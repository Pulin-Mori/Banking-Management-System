#include "common.h"
#include "customer.h"
#include "employee.h"
#include "manager.h"
#include "admin.h"

static int g_listen_fd = -1;

/* Reap terminated client worker processes so they never become zombies */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved_errno;
}

static void sigint_handler(int sig) {
    (void)sig;
    if (g_listen_fd >= 0) close(g_listen_fd);
    _exit(0);
}

/* Update SESSION column ("ONLINE" / "OFFLINE") in customers.csv or employees.csv */
static void set_customer_session_flag(int fd_cust, int slot, const char *state) {
    if (lock_record_data(fd_cust, slot, F_WRLCK, -1, "Customer-Session") < 0) return;
    Customer c;
    if (read_customer_at(fd_cust, slot, &c) == 0) {
        copy_str_safe(c.session, state, sizeof(c.session));
        write_customer_at(fd_cust, slot, &c);
    }
    unlock_record_data(fd_cust, slot);
}

static void set_employee_session_flag(int fd_emp, int slot, const char *state) {
    if (lock_record_data(fd_emp, slot, F_WRLCK, -1, "Employee-Session") < 0) return;
    Employee e;
    if (read_employee_at(fd_emp, slot, &e) == 0) {
        copy_str_safe(e.session, state, sizeof(e.session));
        write_employee_at(fd_emp, slot, &e);
    }
    unlock_record_data(fd_emp, slot);
}

/* Reset any leftover "ONLINE" flags if the server was previously stopped with Ctrl+C */
static void cleanup_stale_sessions_on_boot(int fd_emp, int fd_cust) {
    Employee e;
    for (int slot = 1; slot < get_row_count(fd_emp); slot++) {
        if (read_employee_at(fd_emp, slot, &e) == 0 && strcmp(e.session, "ONLINE") == 0) {
            copy_str_safe(e.session, "OFFLINE", sizeof(e.session));
            write_employee_at(fd_emp, slot, &e);
        }
    }
    Customer c;
    for (int slot = 1; slot < get_row_count(fd_cust); slot++) {
        if (read_customer_at(fd_cust, slot, &c) == 0 && strcmp(c.session, "ONLINE") == 0) {
            copy_str_safe(c.session, "OFFLINE", sizeof(c.session));
            write_customer_at(fd_cust, slot, &c);
        }
    }
}

/* Create and pre-seed all 5 fixed-width 128-byte CSV database files */
static void init_all_databases(void) {
    int fd_emp = open(EMPLOYEES_FILE, O_RDWR | O_CREAT, 0644);
    if (fd_emp >= 0 && lseek(fd_emp, 0, SEEK_END) == 0) {
        write_all(fd_emp, EMPLOYEES_CSV_HEADER, CSV_ROW_SIZE);
        Employee staff[4] = {
            {1001, "Vikram", "Rao",   "admin123", "ADMIN",    "ACTIVE", "OFFLINE", ""},
            {1002, "Meera",  "Iyer",  "mgr123",   "MANAGER",  "ACTIVE", "OFFLINE", ""},
            {1003, "Arjun",  "Patel", "emp123",   "EMPLOYEE", "ACTIVE", "OFFLINE", ""},
            {1004, "Sneha",  "Reddy", "emp123",   "EMPLOYEE", "ACTIVE", "OFFLINE", ""}
        };
        for (int i = 0; i < 4; i++) write_employee_at(fd_emp, i + 1, &staff[i]);
    }

    int fd_cust = open(CUSTOMERS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd_cust >= 0 && lseek(fd_cust, 0, SEEK_END) == 0) {
        write_all(fd_cust, CUSTOMERS_CSV_HEADER, CSV_ROW_SIZE);
        Customer custs[3] = {
            {2001, "Alice", "Sharma", "alice123", 50000.00, 0.00, "ACTIVE", "OFFLINE", ""},
            {2002, "Rohit", "Verma",  "rohit123", 35000.00, 0.00, "ACTIVE", "OFFLINE", ""},
            {2003, "Priya", "Nair",   "priya123", 20000.00, 0.00, "ACTIVE", "OFFLINE", ""}
        };
        for (int i = 0; i < 3; i++) {
            write_customer_at(fd_cust, i + 1, &custs[i]);
            log_transaction(custs[i].userID, "OPEN_ACC", custs[i].balance, custs[i].balance, "Initial Account Opening Deposit");
        }
    }

    cleanup_stale_sessions_on_boot(fd_emp, fd_cust);
    if (fd_emp >= 0)  close(fd_emp);
    if (fd_cust >= 0) close(fd_cust);

    int fd_loan = open(LOANS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd_loan >= 0) {
        if (lseek(fd_loan, 0, SEEK_END) == 0) {
            write_all(fd_loan, LOANS_CSV_HEADER, CSV_ROW_SIZE);
            Loan loans[2] = {
                {3001, 2001, 1003, 15000.00, "ASSIGNED", "Home Renovation Loan", ""},
                {3002, 2002, 0,    25000.00, "PENDING",  "Higher Education Loan", ""}
            };
            for (int i = 0; i < 2; i++) write_loan_at(fd_loan, i + 1, &loans[i]);
        }
        close(fd_loan);
    }

    int fd_fb = open(FEEDBACKS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd_fb >= 0) {
        if (lseek(fd_fb, 0, SEEK_END) == 0) {
            write_all(fd_fb, FEEDBACKS_CSV_HEADER, CSV_ROW_SIZE);
            Feedback fb = {4001, 2001, "Fast online banking service and smooth fund transfer!", "PENDING", ""};
            write_feedback_at(fd_fb, 1, &fb);
        }
        close(fd_fb);
    }
}

/* Authenticate user & acquire non-blocking 1-byte Session Lock on Byte 0 of CSV row */
static int validate_login(int client_fd, int role, int user_id, const char *password,
                          int fd_cust, int fd_emp, int *out_slot, char *out_name, size_t name_len) {
    if (role == ROLE_CUSTOMER) {
        Customer c;
        int slot = find_customer_slot(fd_cust, user_id, &c);
        if (slot < 0 || strcmp(c.password, password) != 0) {
            send_display(client_fd, "\n[AUTH FAILED] Invalid Customer ID or Password.\n");
            return -1;
        }
        if (strcmp(c.status, "ACTIVE") != 0) {
            send_display(client_fd, "\n[LOGIN DENIED] Your Customer Account is currently INACTIVE.\n");
            return -1;
        }
        if (try_acquire_session_lock(fd_cust, slot) == -1) {
            send_display(client_fd, "\n[ONE-SESSION GUARD] Login Rejected! Customer is already logged in on another terminal.\n");
            return -1;
        }
        set_customer_session_flag(fd_cust, slot, "ONLINE");
        *out_slot = slot;
        snprintf(out_name, name_len, "%s %s", c.firstName, c.lastName);
        return 0;
    } else {
        const char *expected_role = (role == ROLE_EMPLOYEE) ? "EMPLOYEE" :
                                    (role == ROLE_MANAGER)  ? "MANAGER"  : "ADMIN";
        Employee e;
        int slot = find_employee_slot(fd_emp, user_id, &e);
        if (slot < 0 || strcmp(e.password, password) != 0 || strcmp(e.role, expected_role) != 0) {
            send_display(client_fd, "\n[AUTH FAILED] Invalid Staff ID, Password, or Role Selection.\n");
            return -1;
        }
        if (try_acquire_session_lock(fd_emp, slot) == -1) {
            send_display(client_fd, "\n[ONE-SESSION GUARD] Login Rejected! Staff member is already logged in on another terminal.\n");
            return -1;
        }
        set_employee_session_flag(fd_emp, slot, "ONLINE");
        *out_slot = slot;
        snprintf(out_name, name_len, "%s %s", e.firstName, e.lastName);
        return 0;
    }
}

/* Worker process loop for a single connected client */
static void handle_client_connection(int client_fd) {
    int fd_cust = open(CUSTOMERS_FILE, O_RDWR);
    int fd_emp  = open(EMPLOYEES_FILE, O_RDWR);
    if (fd_cust < 0 || fd_emp < 0) return;

    while (1) {
        const char *role_banner =
            "\n====================================================================\n"
            "      IIIT-B CS-513 ONLINE BANKING MANAGEMENT SYSTEM (POSIX C)      \n"
            "====================================================================\n"
            "  Select Your Role:\n"
            "    1. Customer\n"
            "    2. Bank Employee\n"
            "    3. Manager\n"
            "    4. Administrator\n"
            "    5. Exit\n"
            "  Enter choice (1-5): ";

        char role_str[MAX_INPUT_LEN], id_str[MAX_INPUT_LEN], password[MAX_INPUT_LEN];
        if (prompt_client(client_fd, role_banner, role_str, sizeof(role_str)) < 0) break;

        int role = atoi(role_str);
        if (role == 5) {
            send_packet(client_fd, MSG_EXIT, "\nThank you for banking with us. Goodbye!\n");
            break;
        }
        if (role < ROLE_CUSTOMER || role > ROLE_ADMIN) {
            send_display(client_fd, "\n[INVALID] Please select a valid role (1-5).\n");
            continue;
        }

        if (prompt_client(client_fd, "  Enter User / Staff ID: ", id_str, sizeof(id_str)) < 0) break;
        if (prompt_client(client_fd, "  Enter Password: ", password, sizeof(password)) < 0) break;

        int slot = -1, user_id = atoi(id_str);
        char full_name[64];
        if (validate_login(client_fd, role, user_id, password, fd_cust, fd_emp,
                           &slot, full_name, sizeof(full_name)) < 0) {
            continue;
        }

        char welcome[200];
        snprintf(welcome, sizeof(welcome),
                 "\n[LOGIN SUCCESS] Welcome, %s (#%06d)! Session locked on Worker PID %d.\n",
                 full_name, user_id, (int)getpid());
        send_display(client_fd, welcome);

        int should_exit = 0;
        if (role == ROLE_CUSTOMER) {
            should_exit = handle_customer_requests(client_fd, fd_cust, slot, user_id);
            set_customer_session_flag(fd_cust, slot, "OFFLINE");
            release_session_lock(fd_cust, slot);
        } else {
            if (role == ROLE_EMPLOYEE)     should_exit = handle_employee_requests(client_fd, fd_emp, fd_cust, slot, user_id);
            else if (role == ROLE_MANAGER) should_exit = handle_manager_requests(client_fd, fd_emp, fd_cust, slot, user_id);
            else                           should_exit = handle_admin_requests(client_fd, fd_emp, fd_cust, slot, user_id);
            set_employee_session_flag(fd_emp, slot, "OFFLINE");
            release_session_lock(fd_emp, slot);
        }

        if (should_exit) {
            send_packet(client_fd, MSG_EXIT, "\nSession closed. Thank you for using the Banking System!\n");
            break;
        }
    }

    close(fd_cust);
    close(fd_emp);
}

int main(int argc, char *argv[]) {
    int port = (argc >= 2) ? atoi(argv[1]) : SERVER_PORT;
    init_all_databases();
    if (argc >= 2 && strcmp(argv[1], "--init-only") == 0) return 0;

    struct sigaction sa_chld = {0}, sa_int = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    sa_int.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa_int, NULL);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port        = htons((uint16_t)port);

    if (bind(g_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 ||
        listen(g_listen_fd, BACKLOG) < 0) {
        perror("bind/listen");
        return 1;
    }

    char banner[200];
    snprintf(banner, sizeof(banner),
             "[SERVER] Banking Server listening on port %d (PID %d, %dB CSV rows)...\n",
             port, (int)getpid(), CSV_ROW_SIZE);
    write_all(STDOUT_FILENO, banner, strlen(banner));

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(g_listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) continue;

        if (fork() == 0) {
            close(g_listen_fd);
            handle_client_connection(client_fd);
            close(client_fd);
            _exit(0);
        }
        close(client_fd);
    }
}