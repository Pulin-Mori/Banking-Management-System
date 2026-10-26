#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define SERVER_IP          "127.0.0.1"
#define SERVER_PORT        8080
#define BACKLOG            16

#define CUSTOMERS_FILE     "customers.csv"
#define EMPLOYEES_FILE     "employees.csv"
#define LOANS_FILE         "loans.csv"
#define TRANSACTIONS_FILE  "transactions.csv"
#define FEEDBACKS_FILE     "feedbacks.csv"

#define CSV_ROW_SIZE       128

#define ROLE_CUSTOMER      1
#define ROLE_EMPLOYEE      2
#define ROLE_MANAGER       3
#define ROLE_ADMIN         4

#define MSG_DISPLAY        1
#define MSG_PROMPT         2
#define MSG_EXIT           3

#define MAX_PAYLOAD        16384
#define MAX_INPUT_LEN      128

typedef struct {
    int msg_type;
    int length;
    char payload[MAX_PAYLOAD];
} NetPacket;

typedef struct {
    int userID;
    char firstName[15], lastName[15], password[16];
    double balance, loan;
    char status[11], session[15], timestamp[21];
} Customer;

typedef struct {
    int empID;
    char firstName[17], lastName[17], password[17];
    char role[15], status[15], session[19], timestamp[21];
} Employee;

typedef struct {
    int loanID, userID, assignedEmpID;
    double amount;
    char status[21], remarks[50], timestamp[21];
} Loan;

typedef struct {
    int transactionID, customerID;
    char type[19];
    double amount, balanceAfter;
    char note[44], timestamp[21];
} Transaction;

typedef struct {
    int feedbackID, customerID;
    char message[73], status[20], timestamp[21];
} Feedback;

/* 128-Byte Fixed-Width CSV Headers */
#define CUSTOMERS_CSV_HEADER \
    "USERID,FIRST_NAME    ,LAST_NAME     ,PASSWORD       ,BALANCE      ,LOAN_AMOUNT  ,STATUS    ,SESSION       ,TIMESTAMP           \n"
#define EMPLOYEES_CSV_HEADER \
    "EMP_ID,FIRST_NAME      ,LAST_NAME       ,PASSWORD        ,ROLE          ,STATUS        ,SESSION           ,TIMESTAMP           \n"
#define LOANS_CSV_HEADER \
    "LOANID,CUSTID,EMP_ID,AMOUNT        ,STATUS              ,REMARKS                                          ,TIMESTAMP           \n"
#define TRANSACTIONS_CSV_HEADER \
    "TXN_ID,CUSTID,TYPE              ,AMOUNT        ,BALANCE_AFTER ,NOTE                                       ,TIMESTAMP           \n"
#define FEEDBACKS_CSV_HEADER \
    "FB_ID ,CUSTID,MESSAGE                                                                 ,STATUS             ,TIMESTAMP           \n"

/* =========================================================================
 * Basic String, Time & POSIX Read/Write Helpers
 * ========================================================================= */
static inline void copy_str_safe(char *dst, const char *src, size_t dst_size) {
    if (dst_size == 0) return;
    size_t i = 0; while (i < dst_size - 1 && src[i]) { dst[i] = src[i]; i++; } dst[i] = 0;
}

static inline void trim_both(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
}

static inline void sanitize_csv_field(char *s) {
    for (int i = 0; s[i] != '\0'; i++) {
        if (s[i] == ',' || s[i] == '\n' || s[i] == '\r') s[i] = ' ';
    }
    trim_both(s);
}

static inline void get_current_timestamp(char out_ts[21]) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(out_ts, 21, "%Y-%m-%d %H:%M:%S", tm_info);
}

static inline ssize_t write_all(int fd, const void *buf, size_t count) {
    size_t written = 0;
    const char *ptr = (const char *)buf;
    while (written < count) {
        ssize_t nw = write(fd, ptr + written, count - written);
        if (nw < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (nw == 0) break;
        written += (size_t)nw;
    }
    return (ssize_t)written;
}

static inline ssize_t read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *ptr = (char *)buf;
    while (total < count) {
        ssize_t nr = read(fd, ptr + total, count - total);
        if (nr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (nr == 0) break;
        total += (size_t)nr;
    }
    return (ssize_t)total;
}

static inline int get_row_count(int fd) {
    return (int)(lseek(fd, 0, SEEK_END) / CSV_ROW_SIZE);
}

static inline int get_next_id(int fd, int base_id) {
    int rows = get_row_count(fd);
    return base_id + ((rows < 1) ? 1 : rows);
}

/* =========================================================================
 * Socket Communication Helpers (send_display & prompt_client)
 * ========================================================================= */
static inline int send_packet(int client_fd, int msg_type, const char *text) {
    NetPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = msg_type;
    size_t len = strlen(text);
    if (len >= MAX_PAYLOAD) len = MAX_PAYLOAD - 1;
    pkt.length = (int)len;
    memcpy(pkt.payload, text, len);
    return (write_all(client_fd, &pkt, sizeof(NetPacket)) == (ssize_t)sizeof(NetPacket)) ? 0 : -1;
}

static inline int send_display(int client_fd, const char *text) {
    return send_packet(client_fd, MSG_DISPLAY, text);
}

static inline int prompt_client(int client_fd, const char *prompt_text, char *reply_buf, size_t reply_size) {
    if (send_packet(client_fd, MSG_PROMPT, prompt_text) < 0) return -1;
    char net_buf[MAX_INPUT_LEN];
    if (read_exact(client_fd, net_buf, sizeof(net_buf)) != (ssize_t)sizeof(net_buf)) return -1;
    net_buf[MAX_INPUT_LEN - 1] = '\0';
    copy_str_safe(reply_buf, net_buf, reply_size);
    trim_both(reply_buf);
    return 0;
}

/* =========================================================================
 * Unified POSIX fcntl() Locking Engine
 *   - Byte 0 of row (offset = slot*128, len = 1)     -> Session Lock (One Login Per User)
 *   - Bytes 1..127  (offset = slot*128 + 1, len=127) -> Data Lock (Read/Write Operations)
 * ========================================================================= */
static inline int apply_fcntl_lock(int fd, short type, off_t start, off_t len, int wait_flag) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;
    fl.l_whence = SEEK_SET;
    fl.l_start  = start;
    fl.l_len    = len;
    fl.l_pid    = getpid();

    int cmd = wait_flag ? F_SETLKW : F_SETLK;
    while (fcntl(fd, cmd, &fl) == -1) {
        if (wait_flag && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static inline int lock_record_data(int fd, int slot, short lock_type, int client_fd, const char *label) {
    off_t start = (off_t)slot * CSV_ROW_SIZE + 1;
    off_t len   = CSV_ROW_SIZE - 1;
    if (apply_fcntl_lock(fd, lock_type, start, len, 0) == -1) {
        if ((errno == EACCES || errno == EAGAIN) && client_fd >= 0) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "\n[LOCK CONTENTION] %s Slot #%d is locked by another process. Waiting via fcntl(F_SETLKW)...\n",
                     label, slot);
            send_display(client_fd, msg);
        }
        return apply_fcntl_lock(fd, lock_type, start, len, 1);
    }
    return 0;
}

static inline int unlock_record_data(int fd, int slot) {
    return apply_fcntl_lock(fd, F_UNLCK, (off_t)slot * CSV_ROW_SIZE + 1, CSV_ROW_SIZE - 1, 0);
}

static inline int lock_whole_file(int fd, short lock_type) {
    return apply_fcntl_lock(fd, lock_type, 0, 0, 1);
}

static inline int unlock_whole_file(int fd) {
    return apply_fcntl_lock(fd, F_UNLCK, 0, 0, 0);
}

static inline int try_acquire_session_lock(int fd, int slot) {
    return apply_fcntl_lock(fd, F_WRLCK, (off_t)slot * CSV_ROW_SIZE, 1, 0);
}

static inline int release_session_lock(int fd, int slot) {
    return apply_fcntl_lock(fd, F_UNLCK, (off_t)slot * CSV_ROW_SIZE, 1, 0);
}

/* =========================================================================
 * Clean 128-Byte CSV Formatters, Parsers & Single-Call Slot Read/Write Helpers
 * ========================================================================= */
static inline void format_customer_row(const Customer *c, char out[CSV_ROW_SIZE + 1]) {
    snprintf(out, CSV_ROW_SIZE + 1,
             "%06d,%-14.14s,%-14.14s,%-15.15s,%013.2f,%013.2f,%-10.10s,%-14.14s,%-20.20s\n",
             c->userID, c->firstName, c->lastName, c->password,
             c->balance, c->loan, c->status, c->session, c->timestamp);
}

static inline int parse_customer_row(const char in[CSV_ROW_SIZE], Customer *c) {
    memset(c, 0, sizeof(*c));
    if (sscanf(in, "%d,%14[^,],%14[^,],%15[^,],%lf,%lf,%10[^,],%14[^,],%20[^\n]",
               &c->userID, c->firstName, c->lastName, c->password,
               &c->balance, &c->loan, c->status, c->session, c->timestamp) != 9) return -1;
    trim_both(c->firstName); trim_both(c->lastName); trim_both(c->password);
    trim_both(c->status);    trim_both(c->session);  trim_both(c->timestamp);
    return (c->userID > 0) ? 0 : -1;
}

static inline void format_employee_row(const Employee *e, char out[CSV_ROW_SIZE + 1]) {
    snprintf(out, CSV_ROW_SIZE + 1,
             "%06d,%-16.16s,%-16.16s,%-16.16s,%-14.14s,%-14.14s,%-18.18s,%-20.20s\n",
             e->empID, e->firstName, e->lastName, e->password,
             e->role, e->status, e->session, e->timestamp);
}

static inline int parse_employee_row(const char in[CSV_ROW_SIZE], Employee *e) {
    memset(e, 0, sizeof(*e));
    if (sscanf(in, "%d,%16[^,],%16[^,],%16[^,],%14[^,],%14[^,],%18[^,],%20[^\n]",
               &e->empID, e->firstName, e->lastName, e->password,
               e->role, e->status, e->session, e->timestamp) != 8) return -1;
    trim_both(e->firstName); trim_both(e->lastName); trim_both(e->password);
    trim_both(e->role);      trim_both(e->status);   trim_both(e->session); trim_both(e->timestamp);
    return (e->empID > 0) ? 0 : -1;
}

static inline void format_loan_row(const Loan *l, char out[CSV_ROW_SIZE + 1]) {
    snprintf(out, CSV_ROW_SIZE + 1,
             "%06d,%06d,%06d,%014.2f,%-20.20s,%-49.49s,%-20.20s\n",
             l->loanID, l->userID, l->assignedEmpID,
             l->amount, l->status, l->remarks, l->timestamp);
}

static inline int parse_loan_row(const char in[CSV_ROW_SIZE], Loan *l) {
    memset(l, 0, sizeof(*l));
    if (sscanf(in, "%d,%d,%d,%lf,%20[^,],%49[^,],%20[^\n]",
               &l->loanID, &l->userID, &l->assignedEmpID,
               &l->amount, l->status, l->remarks, l->timestamp) != 7) return -1;
    trim_both(l->status); trim_both(l->remarks); trim_both(l->timestamp);
    return (l->loanID > 0) ? 0 : -1;
}

static inline void format_transaction_row(const Transaction *t, char out[CSV_ROW_SIZE + 1]) {
    snprintf(out, CSV_ROW_SIZE + 1,
             "%06d,%06d,%-18.18s,%014.2f,%014.2f,%-43.43s,%-20.20s\n",
             t->transactionID, t->customerID, t->type,
             t->amount, t->balanceAfter, t->note, t->timestamp);
}

static inline int parse_transaction_row(const char in[CSV_ROW_SIZE], Transaction *t) {
    memset(t, 0, sizeof(*t));
    if (sscanf(in, "%d,%d,%18[^,],%lf,%lf,%43[^,],%20[^\n]",
               &t->transactionID, &t->customerID, t->type,
               &t->amount, &t->balanceAfter, t->note, t->timestamp) != 7) return -1;
    trim_both(t->type); trim_both(t->note); trim_both(t->timestamp);
    return (t->transactionID > 0) ? 0 : -1;
}

static inline void format_feedback_row(const Feedback *fb, char out[CSV_ROW_SIZE + 1]) {
    snprintf(out, CSV_ROW_SIZE + 1,
             "%06d,%06d,%-72.72s,%-19.19s,%-20.20s\n",
             fb->feedbackID, fb->customerID, fb->message,
             fb->status, fb->timestamp);
}

static inline int parse_feedback_row(const char in[CSV_ROW_SIZE], Feedback *fb) {
    memset(fb, 0, sizeof(*fb));
    if (sscanf(in, "%d,%d,%72[^,],%19[^,],%20[^\n]",
               &fb->feedbackID, &fb->customerID, fb->message,
               fb->status, fb->timestamp) != 5) return -1;
    trim_both(fb->message); trim_both(fb->status); trim_both(fb->timestamp);
    return (fb->feedbackID > 0) ? 0 : -1;
}

/* --- One-Line Slot Read & Write Helpers (Removes Repeated lseek/read/write Code!) --- */
static inline int read_customer_at(int fd, int slot, Customer *c) {
    char raw[CSV_ROW_SIZE];
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    return (read_exact(fd, raw, CSV_ROW_SIZE) == CSV_ROW_SIZE) ? parse_customer_row(raw, c) : -1;
}

static inline void write_customer_at(int fd, int slot, Customer *c) {
    char row[CSV_ROW_SIZE + 1];
    get_current_timestamp(c->timestamp);
    format_customer_row(c, row);
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    write_all(fd, row, CSV_ROW_SIZE);
    fsync(fd);
}

static inline int read_employee_at(int fd, int slot, Employee *e) {
    char raw[CSV_ROW_SIZE];
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    return (read_exact(fd, raw, CSV_ROW_SIZE) == CSV_ROW_SIZE) ? parse_employee_row(raw, e) : -1;
}

static inline void write_employee_at(int fd, int slot, Employee *e) {
    char row[CSV_ROW_SIZE + 1];
    get_current_timestamp(e->timestamp);
    format_employee_row(e, row);
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    write_all(fd, row, CSV_ROW_SIZE);
    fsync(fd);
}

static inline int read_loan_at(int fd, int slot, Loan *l) {
    char raw[CSV_ROW_SIZE];
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    return (read_exact(fd, raw, CSV_ROW_SIZE) == CSV_ROW_SIZE) ? parse_loan_row(raw, l) : -1;
}

static inline void write_loan_at(int fd, int slot, Loan *l) {
    char row[CSV_ROW_SIZE + 1];
    get_current_timestamp(l->timestamp);
    format_loan_row(l, row);
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    write_all(fd, row, CSV_ROW_SIZE);
    fsync(fd);
}

static inline int read_feedback_at(int fd, int slot, Feedback *fb) {
    char raw[CSV_ROW_SIZE];
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    return (read_exact(fd, raw, CSV_ROW_SIZE) == CSV_ROW_SIZE) ? parse_feedback_row(raw, fb) : -1;
}

static inline void write_feedback_at(int fd, int slot, Feedback *fb) {
    char row[CSV_ROW_SIZE + 1];
    get_current_timestamp(fb->timestamp);
    format_feedback_row(fb, row);
    lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
    write_all(fd, row, CSV_ROW_SIZE);
    fsync(fd);
}

/* =========================================================================
 * Transaction Logger & Smart Slot Finders
 * ========================================================================= */
static inline int log_transaction(int customer_id, const char *type, double amount,
                                  double balance_after, const char *note) {
    int fd = open(TRANSACTIONS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;

    lock_whole_file(fd, F_WRLCK);
    if (lseek(fd, 0, SEEK_END) == 0) {
        write_all(fd, TRANSACTIONS_CSV_HEADER, CSV_ROW_SIZE);
    }
    int next_tid = get_row_count(fd);
    Transaction t = {next_tid, customer_id, "", amount, balance_after, "", ""};
    copy_str_safe(t.type, type, sizeof(t.type));
    copy_str_safe(t.note, note, sizeof(t.note));
    get_current_timestamp(t.timestamp);

    char row[CSV_ROW_SIZE + 1];
    format_transaction_row(&t, row);
    lseek(fd, 0, SEEK_END);
    write_all(fd, row, CSV_ROW_SIZE);
    fsync(fd);

    unlock_whole_file(fd);
    close(fd);
    return next_tid;
}

static inline int find_customer_slot(int fd, int target_user_id, Customer *out_cust) {
    int total = get_row_count(fd);
    Customer c;
    for (int slot = 1; slot < total; slot++) {
        if (read_customer_at(fd, slot, &c) == 0 && c.userID == target_user_id &&
            strcmp(c.status, "DELETED") != 0) {
            if (out_cust) *out_cust = c;
            return slot;
        }
    }
    return -1;
}

static inline int find_employee_slot(int fd, int target_emp_id, Employee *out_emp) {
    int total = get_row_count(fd);
    Employee e;
    for (int slot = 1; slot < total; slot++) {
        if (read_employee_at(fd, slot, &e) == 0 && e.empID == target_emp_id &&
            strcmp(e.status, "DELETED") != 0) {
            if (out_emp) *out_emp = e;
            return slot;
        }
    }
    return -1;
}

/* Finds a loan by Loan ID (e.g. 3003) OR Customer ID (e.g. 2001), skipping old APPROVED/REJECTED rows */
static inline int find_assignable_loan_slot(int fd, int id_or_cust_id, Loan *out_loan) {
    int total = get_row_count(fd), fallback_slot = -1;
    Loan l, fallback_loan;
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd, slot, &l) == 0 && l.loanID == id_or_cust_id) {
            if (out_loan) *out_loan = l;
            return slot;
        }
    }
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd, slot, &l) == 0 && l.userID == id_or_cust_id) {
            if (strcmp(l.status, "PENDING") == 0) {
                if (out_loan) *out_loan = l;
                return slot;
            }
            if (strcmp(l.status, "ASSIGNED") == 0 && fallback_slot < 0) {
                fallback_slot = slot;
                fallback_loan = l;
            }
        }
    }
    if (fallback_slot > 0 && out_loan) *out_loan = fallback_loan;
    return fallback_slot;
}

static inline int find_processable_loan_slot(int fd, int id_or_cust_id, int emp_id, Loan *out_loan) {
    int total = get_row_count(fd);
    Loan l;
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd, slot, &l) == 0 && l.loanID == id_or_cust_id) {
            if (out_loan) *out_loan = l;
            return slot;
        }
    }
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd, slot, &l) == 0 && l.userID == id_or_cust_id &&
            strcmp(l.status, "ASSIGNED") == 0 && l.assignedEmpID == emp_id) {
            if (out_loan) *out_loan = l;
            return slot;
        }
    }
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd, slot, &l) == 0 && l.userID == id_or_cust_id &&
            strcmp(l.status, "PENDING") == 0 && l.assignedEmpID == 0) {
            if (out_loan) *out_loan = l;
            return slot;
        }
    }
    return -1;
}

static inline int find_reviewable_feedback_slot(int fd, int id_or_cust_id, Feedback *out_fb) {
    int total = get_row_count(fd);
    Feedback fb;
    for (int slot = 1; slot < total; slot++) {
        if (read_feedback_at(fd, slot, &fb) == 0 && fb.feedbackID == id_or_cust_id) {
            if (out_fb) *out_fb = fb;
            return slot;
        }
    }
    for (int slot = 1; slot < total; slot++) {
        if (read_feedback_at(fd, slot, &fb) == 0 && fb.customerID == id_or_cust_id &&
            strcmp(fb.status, "PENDING") == 0) {
            if (out_fb) *out_fb = fb;
            return slot;
        }
    }
    return -1;
}

static inline int unassign_pending_loans_for_employee(int emp_id) {
    int fd_loan = open(LOANS_FILE, O_RDWR);
    if (fd_loan < 0) return 0;

    lock_whole_file(fd_loan, F_WRLCK);
    int total = get_row_count(fd_loan), reassigned = 0;
    Loan ln;
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd_loan, slot, &ln) == 0 && ln.assignedEmpID == emp_id &&
            strcmp(ln.status, "ASSIGNED") == 0) {
            ln.assignedEmpID = 0;
            copy_str_safe(ln.status, "PENDING", sizeof(ln.status));
            write_loan_at(fd_loan, slot, &ln);
            reassigned++;
        }
    }
    unlock_whole_file(fd_loan);
    close(fd_loan);
    return reassigned;
}

static inline void build_passbook_string(int customer_id, char *out_buf, size_t max_len) {
    int fd = open(TRANSACTIONS_FILE, O_RDONLY);
    if (fd < 0) return;

    lock_whole_file(fd, F_RDLCK);
    int total = get_row_count(fd), count = 0;
    char hdr[320], raw[CSV_ROW_SIZE];
    snprintf(hdr, sizeof(hdr),
             "\n================ PASSBOOK / TRANSACTION HISTORY (Customer #%06d) ================\n"
             "%-6s | %-14s | %-12s | %-12s | %-25s | %-19s\n"
             "---------------------------------------------------------------------------------------------------\n",
             customer_id, "TXN_ID", "TYPE", "AMOUNT (Rs.)", "BAL_AFTER", "NOTE", "TIMESTAMP");
    strncat(out_buf, hdr, max_len - strlen(out_buf) - 1);

    for (int slot = 1; slot < total; slot++) {
        lseek(fd, (off_t)slot * CSV_ROW_SIZE, SEEK_SET);
        if (read_exact(fd, raw, CSV_ROW_SIZE) != CSV_ROW_SIZE) continue;
        Transaction t;
        if (parse_transaction_row(raw, &t) == 0 && t.customerID == customer_id) {
            char line[256];
            snprintf(line, sizeof(line),
                     "%06d | %-14s | %12.2f | %12.2f | %-25.25s | %-19s\n",
                     t.transactionID, t.type, t.amount, t.balanceAfter, t.note, t.timestamp);
            if (strlen(out_buf) + strlen(line) + 64 < max_len) strcat(out_buf, line);
            count++;
        }
    }
    if (count == 0) strncat(out_buf, "  No transactions recorded.\n", max_len - strlen(out_buf) - 1);
    unlock_whole_file(fd);
    close(fd);
}

#endif /* COMMON_H */