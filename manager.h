#ifndef MANAGER_H
#define MANAGER_H

#include "employee.h"

/* 1. Activate / Deactivate Customer Accounts */
static inline void mgr_set_customer_status(int client_fd, int fd_cust) {
    char id_str[MAX_INPUT_LEN], st_str[MAX_INPUT_LEN], prompt_buf[256];
    if (prompt_client(client_fd, "Enter Customer ID to Activate/Deactivate (e.g., 2001): ", id_str, sizeof(id_str)) < 0) return;

    Customer c;
    int slot = find_customer_slot(fd_cust, atoi(id_str), &c);
    if (slot < 0) {
        send_display(client_fd, "[ERROR] Customer ID not found.\n");
        return;
    }

    if (lock_record_data(fd_cust, slot, F_WRLCK, client_fd, "Customer") < 0) return;
    read_customer_at(fd_cust, slot, &c);

    snprintf(prompt_buf, sizeof(prompt_buf),
             "Customer #%06d (%s %s) is [%s]. Select (1 = ACTIVE, 2 = INACTIVE): ",
             c.userID, c.firstName, c.lastName, c.status);
    if (prompt_client(client_fd, prompt_buf, st_str, sizeof(st_str)) < 0) {
        unlock_record_data(fd_cust, slot);
        return;
    }

    int st = atoi(st_str);
    if (st != 1 && st != 2) {
        unlock_record_data(fd_cust, slot);
        send_display(client_fd, "[CANCELLED] Invalid status option.\n");
        return;
    }

    copy_str_safe(c.status, (st == 1) ? "ACTIVE" : "INACTIVE", sizeof(c.status));
    write_customer_at(fd_cust, slot, &c);
    unlock_record_data(fd_cust, slot);

    char msg[200];
    snprintf(msg, sizeof(msg), "[MANAGER SUCCESS] Customer #%06d status set to %s.\n", c.userID, c.status);
    send_display(client_fd, msg);
}

/* 2. Assign Loan Application to Employee (Accepts Loan ID or Customer ID) */
static inline void mgr_assign_loan_to_employee(int client_fd, int fd_emp) {
    int fd_loan = open(LOANS_FILE, O_RDWR);
    if (fd_loan < 0) return;

    lock_whole_file(fd_loan, F_RDLCK);
    int total = get_row_count(fd_loan);
    char list_buf[MAX_PAYLOAD];
    snprintf(list_buf, sizeof(list_buf),
             "\n================ ALL LOAN APPLICATIONS (from loans.csv) ================\n"
             "%-6s | %-6s | %-6s | %-12s | %-10s | %-24s | %-19s\n"
             "--------------------------------------------------------------------------------------------------\n",
             "LOANID", "CUSTID", "EMP_ID", "AMOUNT (Rs.)", "STATUS", "REMARKS", "TIMESTAMP");

    Loan l;
    for (int slot = 1; slot < total; slot++) {
        if (read_loan_at(fd_loan, slot, &l) == 0) {
            char line[200];
            snprintf(line, sizeof(line), "%06d | %06d | %06d | %12.2f | %-10s | %-24.24s | %-19s\n",
                     l.loanID, l.userID, l.assignedEmpID, l.amount, l.status, l.remarks, l.timestamp);
            if (strlen(list_buf) + strlen(line) + 200 < sizeof(list_buf)) strcat(list_buf, line);
        }
    }
    unlock_whole_file(fd_loan);

    strncat(list_buf, "\nEnter Loan ID (e.g., 3002) OR Customer ID (e.g., 2001) to Assign (0 to cancel): ",
            sizeof(list_buf) - strlen(list_buf) - 1);

    char lid_str[MAX_INPUT_LEN], eid_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, list_buf, lid_str, sizeof(lid_str)) < 0 || atoi(lid_str) <= 0) {
        close(fd_loan);
        return;
    }

    Loan ln;
    int lslot = find_assignable_loan_slot(fd_loan, atoi(lid_str), &ln);
    if (lslot < 0 || strcmp(ln.status, "APPROVED") == 0 || strcmp(ln.status, "REJECTED") == 0) {
        close(fd_loan);
        send_display(client_fd, "[ERROR] No assignable PENDING/ASSIGNED loan found for that ID.\n");
        return;
    }

    if (prompt_client(client_fd, "Enter Bank Employee ID to Assign (e.g., 1003 or 1004): ", eid_str, sizeof(eid_str)) < 0) {
        close(fd_loan);
        return;
    }
    Employee emp;
    if (find_employee_slot(fd_emp, atoi(eid_str), &emp) < 0 || strcmp(emp.role, "EMPLOYEE") != 0) {
        close(fd_loan);
        send_display(client_fd, "[ERROR] Active Bank Employee ID not found.\n");
        return;
    }

    if (lock_record_data(fd_loan, lslot, F_WRLCK, client_fd, "Loan") < 0) { close(fd_loan); return; }
    read_loan_at(fd_loan, lslot, &ln);
    ln.assignedEmpID = emp.empID;
    copy_str_safe(ln.status, "ASSIGNED", sizeof(ln.status));
    write_loan_at(fd_loan, lslot, &ln);
    unlock_record_data(fd_loan, lslot);
    close(fd_loan);

    char msg[256];
    snprintf(msg, sizeof(msg), "[MANAGER SUCCESS] Loan #%06d (Customer #%06d) assigned to Employee #%06d (%s %s).\n",
             ln.loanID, ln.userID, emp.empID, emp.firstName, emp.lastName);
    send_display(client_fd, msg);
}

/* 3. Review Customer Feedback */
static inline void mgr_review_feedback(int client_fd) {
    int fd_fb = open(FEEDBACKS_FILE, O_RDWR);
    if (fd_fb < 0) return;

    lock_whole_file(fd_fb, F_RDLCK);
    int total = get_row_count(fd_fb);
    char out[MAX_PAYLOAD];
    snprintf(out, sizeof(out),
             "\n================ CUSTOMER FEEDBACK LOG (from feedbacks.csv) ================\n"
             "%-6s | %-6s | %-10s | %-48s | %-19s\n"
             "---------------------------------------------------------------------------------------------------\n",
             "FB_ID", "CUSTID", "STATUS", "MESSAGE", "TIMESTAMP");

    Feedback fb;
    for (int slot = 1; slot < total; slot++) {
        if (read_feedback_at(fd_fb, slot, &fb) == 0) {
            char line[200];
            snprintf(line, sizeof(line), "%06d | %06d | %-10s | %-48.48s | %-19s\n",
                     fb.feedbackID, fb.customerID, fb.status, fb.message, fb.timestamp);
            if (strlen(out) + strlen(line) + 200 < sizeof(out)) strcat(out, line);
        }
    }
    unlock_whole_file(fd_fb);

    strncat(out, "\nEnter Feedback ID (e.g., 4001) OR Customer ID (e.g., 2001) to mark REVIEWED (0 to return): ",
            sizeof(out) - strlen(out) - 1);

    char fid_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, out, fid_str, sizeof(fid_str)) < 0 || atoi(fid_str) <= 0) {
        close(fd_fb);
        return;
    }

    int fslot = find_reviewable_feedback_slot(fd_fb, atoi(fid_str), &fb);
    if (fslot < 0) {
        close(fd_fb);
        send_display(client_fd, "[ERROR] Matching Feedback not found.\n");
        return;
    }

    if (lock_record_data(fd_fb, fslot, F_WRLCK, client_fd, "Feedback") < 0) { close(fd_fb); return; }
    read_feedback_at(fd_fb, fslot, &fb);
    copy_str_safe(fb.status, "REVIEWED", sizeof(fb.status));
    write_feedback_at(fd_fb, fslot, &fb);
    unlock_record_data(fd_fb, fslot);
    close(fd_fb);

    char msg[200];
    snprintf(msg, sizeof(msg), "[MANAGER SUCCESS] Feedback #%06d (Customer #%06d) marked REVIEWED.\n",
             fb.feedbackID, fb.customerID);
    send_display(client_fd, msg);
}

/* Manager Session Loop: Returns 0 on Logout, 1 on Exit */
static inline int handle_manager_requests(int client_fd, int fd_emp, int fd_cust, int mgr_slot, int mgr_id) {
    while (1) {
        char menu[768], choice_str[MAX_INPUT_LEN];
        snprintf(menu, sizeof(menu),
                 "\n====================================================================\n"
                 "  BANK MANAGER PORTAL [Manager ID: %06d | CSV Slot #%d | ONLINE]\n"
                 "====================================================================\n"
                 "  1. Activate / Deactivate Customer Account\n"
                 "  2. Assign Loan Application Process to Employee\n"
                 "  3. Review Customer Feedback\n"
                 "  4. Change Password\n"
                 "  5. Logout (Return to Role Selection)\n"
                 "  6. Exit (Disconnect Client)\n"
                 "Enter choice (1-6): ",
                 mgr_id, mgr_slot);

        if (prompt_client(client_fd, menu, choice_str, sizeof(choice_str)) < 0) return 1;
        switch (atoi(choice_str)) {
            case 1: mgr_set_customer_status(client_fd, fd_cust); break;
            case 2: mgr_assign_loan_to_employee(client_fd, fd_emp); break;
            case 3: mgr_review_feedback(client_fd); break;
            case 4: change_staff_password(client_fd, fd_emp, mgr_slot); break;
            case 5:
                send_display(client_fd, "\n[LOGOUT] Logging out of Bank Manager session...\n");
                return 0;
            case 6: return 1;
            default: send_display(client_fd, "[INVALID] Please enter a valid option (1-6).\n"); break;
        }
    }
}

#endif /* MANAGER_H */