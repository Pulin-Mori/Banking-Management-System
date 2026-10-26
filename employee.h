#ifndef EMPLOYEE_H
#define EMPLOYEE_H

#include "common.h"

/* 1. Add New Customer */
static inline void emp_add_customer(int client_fd, int fd_cust) {
    char fname[MAX_INPUT_LEN], lname[MAX_INPUT_LEN], pass[MAX_INPUT_LEN], bal_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter Customer First Name: ", fname, sizeof(fname)) < 0) return;
    if (prompt_client(client_fd, "Enter Customer Last Name: ",  lname, sizeof(lname)) < 0) return;
    if (prompt_client(client_fd, "Enter Initial Password: ",    pass,  sizeof(pass))  < 0) return;
    sanitize_csv_field(fname); sanitize_csv_field(lname); sanitize_csv_field(pass);
    if (strlen(fname) == 0 || strlen(pass) == 0) {
        send_display(client_fd, "[CANCELLED] First Name and Password cannot be empty.\n");
        return;
    }
    if (prompt_client(client_fd, "Enter Opening Balance (Rs.): ", bal_str, sizeof(bal_str)) < 0) return;
    double open_bal = atof(bal_str) < 0.0 ? 0.0 : atof(bal_str);

    lock_whole_file(fd_cust, F_WRLCK);
    int new_id = get_next_id(fd_cust, 2000);
    Customer c = {new_id, "", "", "", open_bal, 0.0, "ACTIVE", "OFFLINE", ""};
    copy_str_safe(c.firstName, fname, sizeof(c.firstName));
    copy_str_safe(c.lastName,  lname, sizeof(c.lastName));
    copy_str_safe(c.password,  pass,  sizeof(c.password));
    write_customer_at(fd_cust, get_row_count(fd_cust), &c);
    unlock_whole_file(fd_cust);

    log_transaction(new_id, "OPEN_ACC", open_bal, open_bal, "Account Opened by Employee");
    char msg[256];
    snprintf(msg, sizeof(msg), "\n[EMPLOYEE SUCCESS] Added Customer #%06d (%s %s) with Balance Rs. %.2f.\n",
             new_id, c.firstName, c.lastName, c.balance);
    send_display(client_fd, msg);
}

/* 2. Modify Customer Details */
static inline void emp_update_customer_details(int client_fd, int fd_cust) {
    char id_str[MAX_INPUT_LEN], fn[MAX_INPUT_LEN], ln[MAX_INPUT_LEN], pw[MAX_INPUT_LEN], prompt_buf[200];
    if (prompt_client(client_fd, "Enter Customer ID to Modify (e.g., 2001): ", id_str, sizeof(id_str)) < 0) return;

    Customer c;
    int slot = find_customer_slot(fd_cust, atoi(id_str), &c);
    if (slot < 0) {
        send_display(client_fd, "[ERROR] Customer ID not found.\n");
        return;
    }
    if (lock_record_data(fd_cust, slot, F_WRLCK, client_fd, "Customer") < 0) return;
    read_customer_at(fd_cust, slot, &c);

    snprintf(prompt_buf, sizeof(prompt_buf), "First Name [%s] (ENTER to keep): ", c.firstName);
    if (prompt_client(client_fd, prompt_buf, fn, sizeof(fn)) < 0) { unlock_record_data(fd_cust, slot); return; }
    snprintf(prompt_buf, sizeof(prompt_buf), "Last Name  [%s] (ENTER to keep): ", c.lastName);
    if (prompt_client(client_fd, prompt_buf, ln, sizeof(ln)) < 0) { unlock_record_data(fd_cust, slot); return; }
    snprintf(prompt_buf, sizeof(prompt_buf), "Password   [%s] (ENTER to keep): ", c.password);
    if (prompt_client(client_fd, prompt_buf, pw, sizeof(pw)) < 0) { unlock_record_data(fd_cust, slot); return; }

    sanitize_csv_field(fn); sanitize_csv_field(ln); sanitize_csv_field(pw);
    if (strlen(fn) > 0) copy_str_safe(c.firstName, fn, sizeof(c.firstName));
    if (strlen(ln) > 0) copy_str_safe(c.lastName,  ln, sizeof(c.lastName));
    if (strlen(pw) > 0) copy_str_safe(c.password,  pw, sizeof(c.password));

    write_customer_at(fd_cust, slot, &c);
    unlock_record_data(fd_cust, slot);
    send_display(client_fd, "[SUCCESS] Customer details updated in customers.csv.\n");
}

/* Helper to build the Employee's loan queue table */
static inline int build_employee_loan_queue_string(int fd_loan, int emp_id, char *out_buf, size_t max_len) {
    lock_whole_file(fd_loan, F_RDLCK);
    int total = get_row_count(fd_loan), actionable = 0, hist = 0;

    snprintf(out_buf, max_len,
             "\n================ ACTIONABLE LOAN APPLICATIONS FOR EMPLOYEE #%06d ================\n"
             "%-6s | %-6s | %-6s | %-12s | %-10s | %-24s | %-19s\n"
             "--------------------------------------------------------------------------------------------------\n",
             emp_id, "LOANID", "CUSTID", "EMP_ID", "AMOUNT (Rs.)", "STATUS", "REMARKS", "TIMESTAMP");

    Loan l;
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd_loan, slot, &l) != 0) continue;
        if ((l.assignedEmpID == emp_id && strcmp(l.status, "ASSIGNED") == 0) ||
            (l.assignedEmpID == 0 && strcmp(l.status, "PENDING") == 0)) {
            char line[200];
            snprintf(line, sizeof(line), "%06d | %06d | %06d | %12.2f | %-10s | %-24.24s | %-19s\n",
                     l.loanID, l.userID, l.assignedEmpID, l.amount, l.status, l.remarks, l.timestamp);
            if (strlen(out_buf) + strlen(line) + 128 < max_len) strcat(out_buf, line);
            actionable++;
        }
    }
    if (actionable == 0) strncat(out_buf, "  (No active ASSIGNED or PENDING loans right now)\n", max_len - strlen(out_buf) - 1);

    strncat(out_buf, "\n---------------- COMPLETED LOAN HISTORY (APPROVED / REJECTED) ----------------\n",
            max_len - strlen(out_buf) - 1);
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd_loan, slot, &l) != 0) continue;
        if (l.assignedEmpID == emp_id && (strcmp(l.status, "APPROVED") == 0 || strcmp(l.status, "REJECTED") == 0)) {
            char line[200];
            snprintf(line, sizeof(line), "%06d | %06d | %06d | %12.2f | %-10s | %-24.24s | %-19s\n",
                     l.loanID, l.userID, l.assignedEmpID, l.amount, l.status, l.remarks, l.timestamp);
            if (strlen(out_buf) + strlen(line) + 128 < max_len) strcat(out_buf, line);
            hist++;
        }
    }
    if (hist == 0) strncat(out_buf, "  (No completed loans yet)\n", max_len - strlen(out_buf) - 1);

    unlock_whole_file(fd_loan);
    return actionable;
}

/* 3. View Assigned & Pending Loan Applications */
static inline void emp_get_assigned_loans(int client_fd, int emp_id) {
    int fd_loan = open(LOANS_FILE, O_RDONLY);
    if (fd_loan < 0) return;
    char out[MAX_PAYLOAD];
    build_employee_loan_queue_string(fd_loan, emp_id, out, sizeof(out));
    close(fd_loan);
    send_display(client_fd, out);
}

/* 4. Approve / Reject Loan Application (Accepts Loan ID or Customer ID) */
static inline void emp_update_loan_status(int client_fd, int fd_cust, int emp_id) {
    int fd_loan = open(LOANS_FILE, O_RDWR);
    if (fd_loan < 0) return;

    char queue_buf[MAX_PAYLOAD], id_str[MAX_INPUT_LEN], dec_str[MAX_INPUT_LEN];
    if (build_employee_loan_queue_string(fd_loan, emp_id, queue_buf, sizeof(queue_buf) - 200) == 0) {
        close(fd_loan);
        send_display(client_fd, queue_buf);
        return;
    }

    strncat(queue_buf, "\nEnter Loan ID (e.g., 3001) OR Customer ID (e.g., 2001) to Process (0 to cancel): ",
            sizeof(queue_buf) - strlen(queue_buf) - 1);
    if (prompt_client(client_fd, queue_buf, id_str, sizeof(id_str)) < 0 || atoi(id_str) <= 0) {
        close(fd_loan);
        return;
    }

    Loan ln;
    int lslot = find_processable_loan_slot(fd_loan, atoi(id_str), emp_id, &ln);
    if (lslot < 0) {
        close(fd_loan);
        send_display(client_fd, "[ERROR] No active ASSIGNED or PENDING loan found for that ID.\n");
        return;
    }

    if (lock_record_data(fd_loan, lslot, F_WRLCK, client_fd, "Loan") < 0) { close(fd_loan); return; }
    read_loan_at(fd_loan, lslot, &ln);

    if ((ln.assignedEmpID != 0 && ln.assignedEmpID != emp_id) ||
        strcmp(ln.status, "APPROVED") == 0 || strcmp(ln.status, "REJECTED") == 0) {
        unlock_record_data(fd_loan, lslot);
        close(fd_loan);
        send_display(client_fd, "[DENIED] Loan is either assigned to another employee or already processed.\n");
        return;
    }

    char prompt_msg[300];
    snprintf(prompt_msg, sizeof(prompt_msg),
             "\nLoan #%06d | Customer #%06d | Amount: Rs. %.2f | Purpose: %s\n"
             "Enter Decision (1 = APPROVE, 2 = REJECT, 0 = Cancel): ",
             ln.loanID, ln.userID, ln.amount, ln.remarks);
    if (prompt_client(client_fd, prompt_msg, dec_str, sizeof(dec_str)) < 0) {
        unlock_record_data(fd_loan, lslot); close(fd_loan); return;
    }

    int decision = atoi(dec_str);
    ln.assignedEmpID = emp_id;

    if (decision == 1) {
        Customer cust;
        int cslot = find_customer_slot(fd_cust, ln.userID, &cust);
        if (cslot < 0 || lock_record_data(fd_cust, cslot, F_WRLCK, client_fd, "Customer-Loan") < 0) {
            unlock_record_data(fd_loan, lslot); close(fd_loan); return;
        }
        read_customer_at(fd_cust, cslot, &cust);
        cust.balance += ln.amount;
        cust.loan    += ln.amount;
        write_customer_at(fd_cust, cslot, &cust);
        unlock_record_data(fd_cust, cslot);

        copy_str_safe(ln.status, "APPROVED", sizeof(ln.status));
        write_loan_at(fd_loan, lslot, &ln);
        unlock_record_data(fd_loan, lslot);
        close(fd_loan);

        char note[44], ok_msg[384];
        snprintf(note, sizeof(note), "Loan #%06d Approved by Emp #%06d", ln.loanID, emp_id);
        log_transaction(cust.userID, "LOAN_CREDIT", ln.amount, cust.balance, note);

        snprintf(ok_msg, sizeof(ok_msg),
                 "\n[LOAN APPROVED] Loan #%06d credited Rs. %.2f to Customer #%06d. New Balance: Rs. %.2f | Active Loan: Rs. %.2f\n",
                 ln.loanID, ln.amount, cust.userID, cust.balance, cust.loan);
        send_display(client_fd, ok_msg);
    } else if (decision == 2) {
        copy_str_safe(ln.status, "REJECTED", sizeof(ln.status));
        write_loan_at(fd_loan, lslot, &ln);
        unlock_record_data(fd_loan, lslot);
        close(fd_loan);

        char rej_msg[200];
        snprintf(rej_msg, sizeof(rej_msg), "[LOAN REJECTED] Loan #%06d for Customer #%06d marked REJECTED.\n",
                 ln.loanID, ln.userID);
        send_display(client_fd, rej_msg);
    } else {
        unlock_record_data(fd_loan, lslot);
        close(fd_loan);
        send_display(client_fd, "[CANCELLED] Loan processing aborted.\n");
    }
}

/* 5. View Customer Transactions (Passbook) */
static inline void emp_view_customer_transactions(int client_fd, int fd_cust) {
    char id_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter Customer ID to View Passbook (e.g., 2001): ", id_str, sizeof(id_str)) < 0) return;

    Customer c;
    if (find_customer_slot(fd_cust, atoi(id_str), &c) < 0) {
        send_display(client_fd, "[ERROR] Customer ID not found.\n");
        return;
    }
    char out[MAX_PAYLOAD];
    snprintf(out, sizeof(out), "\n--- CUSTOMER #%06d (%s %s) | Status: %s | Bal: Rs. %.2f | Loan: Rs. %.2f ---\n",
             c.userID, c.firstName, c.lastName, c.status, c.balance, c.loan);
    build_passbook_string(c.userID, out, sizeof(out));
    send_display(client_fd, out);
}

/* 6. Change Staff Password (Shared by Employee, Manager & Admin) */
static inline void change_staff_password(int client_fd, int fd_emp, int slot) {
    char new_pw[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter New Password (max 16 chars): ", new_pw, sizeof(new_pw)) < 0) return;
    sanitize_csv_field(new_pw);
    if (strlen(new_pw) == 0) return;

    if (lock_record_data(fd_emp, slot, F_WRLCK, client_fd, "Employee") < 0) return;
    Employee e;
    read_employee_at(fd_emp, slot, &e);
    copy_str_safe(e.password, new_pw, sizeof(e.password));
    write_employee_at(fd_emp, slot, &e);
    unlock_record_data(fd_emp, slot);
    send_display(client_fd, "[SUCCESS] Password updated in employees.csv.\n");
}

/* Bank Employee Session Loop: Returns 0 on Logout, 1 on Exit */
static inline int handle_employee_requests(int client_fd, int fd_emp, int fd_cust, int emp_slot, int emp_id) {
    while (1) {
        char menu[768], choice_str[MAX_INPUT_LEN];
        snprintf(menu, sizeof(menu),
                 "\n====================================================================\n"
                 "  BANK EMPLOYEE PORTAL [Employee ID: %06d | CSV Slot #%d | ONLINE]\n"
                 "====================================================================\n"
                 "  1. Add New Customer\n"
                 "  2. Modify Customer Details\n"
                 "  3. View Assigned & Pending Loan Applications\n"
                 "  4. Approve / Reject Loan Application\n"
                 "  5. View Customer Transactions (Passbook)\n"
                 "  6. Change Password\n"
                 "  7. Logout (Return to Role Selection)\n"
                 "  8. Exit (Disconnect Client)\n"
                 "Enter choice (1-8): ",
                 emp_id, emp_slot);

        if (prompt_client(client_fd, menu, choice_str, sizeof(choice_str)) < 0) return 1;
        switch (atoi(choice_str)) {
            case 1: emp_add_customer(client_fd, fd_cust); break;
            case 2: emp_update_customer_details(client_fd, fd_cust); break;
            case 3: emp_get_assigned_loans(client_fd, emp_id); break;
            case 4: emp_update_loan_status(client_fd, fd_cust, emp_id); break;
            case 5: emp_view_customer_transactions(client_fd, fd_cust); break;
            case 6: change_staff_password(client_fd, fd_emp, emp_slot); break;
            case 7:
                send_display(client_fd, "\n[LOGOUT] Logging out of Bank Employee session...\n");
                return 0;
            case 8: return 1;
            default: send_display(client_fd, "[INVALID] Please enter a valid option (1-8).\n"); break;
        }
    }
}

#endif /* EMPLOYEE_H */