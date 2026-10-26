#ifndef ADMIN_H
#define ADMIN_H

#include "manager.h"

/* 1. Add New Bank Employee or Manager */
static inline void admin_add_employee(int client_fd, int fd_emp) {
    char fname[MAX_INPUT_LEN], lname[MAX_INPUT_LEN], pass[MAX_INPUT_LEN], role_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter Staff First Name: ", fname, sizeof(fname)) < 0) return;
    if (prompt_client(client_fd, "Enter Staff Last Name: ",  lname, sizeof(lname)) < 0) return;
    if (prompt_client(client_fd, "Enter Initial Password: ", pass,  sizeof(pass))  < 0) return;
    sanitize_csv_field(fname); sanitize_csv_field(lname); sanitize_csv_field(pass);
    if (strlen(fname) == 0 || strlen(pass) == 0) {
        send_display(client_fd, "[CANCELLED] First Name and Password cannot be empty.\n");
        return;
    }
    if (prompt_client(client_fd, "Select Role (1 = EMPLOYEE, 2 = MANAGER): ", role_str, sizeof(role_str)) < 0) return;

    lock_whole_file(fd_emp, F_WRLCK);
    int new_id = get_next_id(fd_emp, 1000);
    Employee e = {new_id, "", "", "", "", "ACTIVE", "OFFLINE", ""};
    copy_str_safe(e.firstName, fname, sizeof(e.firstName));
    copy_str_safe(e.lastName,  lname, sizeof(e.lastName));
    copy_str_safe(e.password,  pass,  sizeof(e.password));
    copy_str_safe(e.role, (atoi(role_str) == 2) ? "MANAGER" : "EMPLOYEE", sizeof(e.role));
    write_employee_at(fd_emp, get_row_count(fd_emp), &e);
    unlock_whole_file(fd_emp);

    char msg[256];
    snprintf(msg, sizeof(msg), "\n[ADMIN SUCCESS] Added %s #%06d (%s %s) in employees.csv.\n",
             e.role, e.empID, e.firstName, e.lastName);
    send_display(client_fd, msg);
}

/* 2. Modify Customer / Employee Details */
static inline void admin_modify_details(int client_fd, int fd_emp, int fd_cust) {
    char target_type[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Modify (1 = Customer Details, 2 = Employee/Manager Details): ",
                      target_type, sizeof(target_type)) < 0) return;

    if (atoi(target_type) == 1) {
        emp_update_customer_details(client_fd, fd_cust);
    } else if (atoi(target_type) == 2) {
        char id_str[MAX_INPUT_LEN], fn[MAX_INPUT_LEN], ln[MAX_INPUT_LEN], pw[MAX_INPUT_LEN], prompt_buf[200];
        if (prompt_client(client_fd, "Enter Employee/Manager ID to Modify: ", id_str, sizeof(id_str)) < 0) return;

        Employee e;
        int slot = find_employee_slot(fd_emp, atoi(id_str), &e);
        if (slot < 0) {
            send_display(client_fd, "[ERROR] Staff ID not found.\n");
            return;
        }
        if (lock_record_data(fd_emp, slot, F_WRLCK, client_fd, "Employee") < 0) return;
        read_employee_at(fd_emp, slot, &e);

        snprintf(prompt_buf, sizeof(prompt_buf), "First Name [%s] (ENTER to keep): ", e.firstName);
        if (prompt_client(client_fd, prompt_buf, fn, sizeof(fn)) < 0) { unlock_record_data(fd_emp, slot); return; }
        snprintf(prompt_buf, sizeof(prompt_buf), "Last Name  [%s] (ENTER to keep): ", e.lastName);
        if (prompt_client(client_fd, prompt_buf, ln, sizeof(ln)) < 0) { unlock_record_data(fd_emp, slot); return; }
        snprintf(prompt_buf, sizeof(prompt_buf), "Password   [%s] (ENTER to keep): ", e.password);
        if (prompt_client(client_fd, prompt_buf, pw, sizeof(pw)) < 0) { unlock_record_data(fd_emp, slot); return; }

        sanitize_csv_field(fn); sanitize_csv_field(ln); sanitize_csv_field(pw);
        if (strlen(fn) > 0) copy_str_safe(e.firstName, fn, sizeof(e.firstName));
        if (strlen(ln) > 0) copy_str_safe(e.lastName,  ln, sizeof(e.lastName));
        if (strlen(pw) > 0) copy_str_safe(e.password,  pw, sizeof(e.password));

        write_employee_at(fd_emp, slot, &e);
        unlock_record_data(fd_emp, slot);
        send_display(client_fd, "[ADMIN SUCCESS] Employee details updated in employees.csv.\n");
    }
}

/* 3. Manage User Roles (Promote Employee to Manager / Demote Manager to Employee) */
static inline void admin_manage_user_roles(int client_fd, int fd_emp) {
    char id_str[MAX_INPUT_LEN], role_choice[MAX_INPUT_LEN], prompt_buf[256];
    if (prompt_client(client_fd, "Enter Employee/Manager ID to Change Role (e.g., 1003): ", id_str, sizeof(id_str)) < 0) return;

    Employee e;
    int slot = find_employee_slot(fd_emp, atoi(id_str), &e);
    if (slot < 0 || strcmp(e.role, "ADMIN") == 0) {
        send_display(client_fd, "[ERROR] Valid non-admin Staff ID not found.\n");
        return;
    }

    if (lock_record_data(fd_emp, slot, F_WRLCK, client_fd, "Employee") < 0) return;
    read_employee_at(fd_emp, slot, &e);

    snprintf(prompt_buf, sizeof(prompt_buf),
             "Staff #%06d (%s %s) is [%s]. Select (1 = Promote to MANAGER, 2 = Demote to EMPLOYEE): ",
             e.empID, e.firstName, e.lastName, e.role);
    if (prompt_client(client_fd, prompt_buf, role_choice, sizeof(role_choice)) < 0) {
        unlock_record_data(fd_emp, slot);
        return;
    }

    int act = atoi(role_choice), released = 0;
    if (act == 1) {
        copy_str_safe(e.role, "MANAGER", sizeof(e.role));
        released = unassign_pending_loans_for_employee(e.empID);
    } else if (act == 2) {
        copy_str_safe(e.role, "EMPLOYEE", sizeof(e.role));
    } else {
        unlock_record_data(fd_emp, slot);
        return;
    }

    write_employee_at(fd_emp, slot, &e);
    unlock_record_data(fd_emp, slot);

    char msg[300];
    snprintf(msg, sizeof(msg),
             "[ADMIN SUCCESS] Staff #%06d (%s %s) role updated to %s (%d assigned loans returned to PENDING).\n",
             e.empID, e.firstName, e.lastName, e.role, released);
    send_display(client_fd, msg);
}

/* 4. Delete Customer or Employee Account */
static inline void admin_delete_account(int client_fd, int fd_emp, int fd_cust) {
    char target_type[MAX_INPUT_LEN], id_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Delete (1 = Customer Account, 2 = Employee Account): ", target_type, sizeof(target_type)) < 0) return;
    int t = atoi(target_type);

    if (t == 1) {
        if (prompt_client(client_fd, "Enter Customer ID to Delete: ", id_str, sizeof(id_str)) < 0) return;
        Customer c;
        int slot = find_customer_slot(fd_cust, atoi(id_str), &c);
        if (slot < 0 || lock_record_data(fd_cust, slot, F_WRLCK, client_fd, "Customer") < 0) {
            send_display(client_fd, "[ERROR] Customer ID not found.\n");
            return;
        }
        read_customer_at(fd_cust, slot, &c);
        copy_str_safe(c.status, "DELETED", sizeof(c.status));
        write_customer_at(fd_cust, slot, &c);
        unlock_record_data(fd_cust, slot);
        send_display(client_fd, "[ADMIN SUCCESS] Customer marked DELETED in customers.csv.\n");
    } else if (t == 2) {
        if (prompt_client(client_fd, "Enter Employee ID to Delete: ", id_str, sizeof(id_str)) < 0) return;
        Employee e;
        int slot = find_employee_slot(fd_emp, atoi(id_str), &e);
        if (slot < 0 || strcmp(e.role, "ADMIN") == 0 || lock_record_data(fd_emp, slot, F_WRLCK, client_fd, "Employee") < 0) {
            send_display(client_fd, "[ERROR] Valid non-admin Employee ID not found.\n");
            return;
        }
        read_employee_at(fd_emp, slot, &e);
        copy_str_safe(e.status, "DELETED", sizeof(e.status));
        write_employee_at(fd_emp, slot, &e);
        unlock_record_data(fd_emp, slot);
        int released = unassign_pending_loans_for_employee(e.empID);
        char msg[240];
        snprintf(msg, sizeof(msg), "[ADMIN SUCCESS] Staff #%06d marked DELETED (%d assigned loans returned to PENDING).\n",
                 e.empID, released);
        send_display(client_fd, msg);
    }
}

/* 5. View All Customers & Staff Directory */
static inline void admin_view_directory(int client_fd, int fd_emp, int fd_cust) {
    char out[MAX_PAYLOAD];
    snprintf(out, sizeof(out),
             "\n================ STAFF DIRECTORY (employees.csv, 128B/row) ================\n"
             "%-6s | %-12s | %-12s | %-10s | %-8s | %-8s | %-19s\n"
             "-------------------------------------------------------------------------------------------\n",
             "EMP_ID", "FIRST_NAME", "LAST_NAME", "ROLE", "STATUS", "SESSION", "TIMESTAMP");

    lock_whole_file(fd_emp, F_RDLCK);
    int erows = get_row_count(fd_emp);
    Employee e;
    for (int slot = 1; slot < erows; slot++) {
        if (read_employee_at(fd_emp, slot, &e) == 0) {
            char line[180];
            snprintf(line, sizeof(line), "%06d | %-12s | %-12s | %-10s | %-8s | %-8s | %-19s\n",
                     e.empID, e.firstName, e.lastName, e.role, e.status, e.session, e.timestamp);
            if (strlen(out) + strlen(line) + 128 < sizeof(out)) strcat(out, line);
        }
    }
    unlock_whole_file(fd_emp);

    char cust_hdr[400];
    snprintf(cust_hdr, sizeof(cust_hdr),
             "\n================ CUSTOMER DIRECTORY (customers.csv, 128B/row) ================\n"
             "%-6s | %-12s | %-12s | %-11s | %-11s | %-8s | %-7s | %-19s\n"
             "--------------------------------------------------------------------------------------------------------\n",
             "USERID", "FIRST_NAME", "LAST_NAME", "BALANCE", "LOAN", "STATUS", "SESSION", "TIMESTAMP");
    strncat(out, cust_hdr, sizeof(out) - strlen(out) - 1);

    lock_whole_file(fd_cust, F_RDLCK);
    int crows = get_row_count(fd_cust);
    Customer c;
    for (int slot = 1; slot < crows; slot++) {
        if (read_customer_at(fd_cust, slot, &c) == 0) {
            char line[200];
            snprintf(line, sizeof(line), "%06d | %-12s | %-12s | %11.2f | %11.2f | %-8s | %-7s | %-19s\n",
                     c.userID, c.firstName, c.lastName, c.balance, c.loan, c.status, c.session, c.timestamp);
            if (strlen(out) + strlen(line) + 64 < sizeof(out)) strcat(out, line);
        }
    }
    unlock_whole_file(fd_cust);
    send_display(client_fd, out);
}

/* Administrator Session Loop: Returns 0 on Logout, 1 on Exit */
static inline int handle_admin_requests(int client_fd, int fd_emp, int fd_cust, int admin_slot, int admin_id) {
    while (1) {
        char menu[768], choice_str[MAX_INPUT_LEN];
        snprintf(menu, sizeof(menu),
                 "\n====================================================================\n"
                 "  ADMINISTRATOR PORTAL [Admin ID: %06d | CSV Slot #%d | ONLINE]\n"
                 "====================================================================\n"
                 "  1. Add New Bank Employee / Manager\n"
                 "  2. Modify Customer / Employee Details\n"
                 "  3. Manage User Roles (Promote Employee / Demote Manager)\n"
                 "  4. Delete Customer / Employee Account\n"
                 "  5. View All Staff & Customer Directory (with Session & Timestamp)\n"
                 "  6. Change Password\n"
                 "  7. Logout (Return to Role Selection)\n"
                 "  8. Exit (Disconnect Client)\n"
                 "Enter choice (1-8): ",
                 admin_id, admin_slot);

        if (prompt_client(client_fd, menu, choice_str, sizeof(choice_str)) < 0) return 1;
        switch (atoi(choice_str)) {
            case 1: admin_add_employee(client_fd, fd_emp); break;
            case 2: admin_modify_details(client_fd, fd_emp, fd_cust); break;
            case 3: admin_manage_user_roles(client_fd, fd_emp); break;
            case 4: admin_delete_account(client_fd, fd_emp, fd_cust); break;
            case 5: admin_view_directory(client_fd, fd_emp, fd_cust); break;
            case 6: change_staff_password(client_fd, fd_emp, admin_slot); break;
            case 7:
                send_display(client_fd, "\n[LOGOUT] Logging out of Administrator session...\n");
                return 0;
            case 8: return 1;
            default: send_display(client_fd, "[INVALID] Please enter a valid option (1-8).\n"); break;
        }
    }
}

#endif /* ADMIN_H */