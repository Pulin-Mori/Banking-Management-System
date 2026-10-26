#ifndef CUSTOMER_H
#define CUSTOMER_H

#include "common.h"

/* Verify customer account is still ACTIVE before any mutating operation */
static inline int verify_customer_active(int client_fd, int fd_cust, int slot, Customer *out_c) {
    if (lock_record_data(fd_cust, slot, F_RDLCK, client_fd, "Customer") < 0) return -1;
    read_customer_at(fd_cust, slot, out_c);
    unlock_record_data(fd_cust, slot);

    if (strcmp(out_c->status, "ACTIVE") != 0) {
        send_display(client_fd, "\n[ACCOUNT RESTRICTED] Your account is INACTIVE or DELETED. Contact Bank Manager.\n");
        return -1;
    }
    return 0;
}

/* 1. View Account Balance (Shared Read Lock F_RDLCK) */
static inline void get_balance(int client_fd, int fd_cust, int slot) {
    if (lock_record_data(fd_cust, slot, F_RDLCK, client_fd, "Customer") < 0) return;
    Customer c;
    read_customer_at(fd_cust, slot, &c);
    unlock_record_data(fd_cust, slot);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "\n[READ LOCK (F_RDLCK) ACQUIRED & RELEASED on Slot #%d]\n"
             "  Customer ID      : %06d (%s %s)\n"
             "  Account Status   : %s\n"
             "  Available Balance: Rs. %.2f\n"
             "  Active Loan Debt : Rs. %.2f\n"
             "  Last Updated     : %s\n",
             slot, c.userID, c.firstName, c.lastName, c.status, c.balance, c.loan, c.timestamp);
    send_display(client_fd, msg);
}

/* 2 & 3. Unified Deposit / Withdraw Helper (Avoids Duplicating Lock/Read/Write Code) */
static inline void update_customer_balance(int client_fd, int fd_cust, int slot, int customer_id, int is_withdraw) {
    if (lock_record_data(fd_cust, slot, F_WRLCK, client_fd, "Customer") < 0) return;

    Customer c;
    read_customer_at(fd_cust, slot, &c);
    if (strcmp(c.status, "ACTIVE") != 0) {
        unlock_record_data(fd_cust, slot);
        send_display(client_fd, "[REJECTED] Your account is INACTIVE. Contact Bank Manager.\n");
        return;
    }

    char prompt_msg[256], amt_str[MAX_INPUT_LEN];
    snprintf(prompt_msg, sizeof(prompt_msg),
             "\n[WRITE LOCK (F_WRLCK) HELD on Customer #%06d | Balance: Rs. %.2f]\n"
             "Enter amount to %s (Rs.): ",
             customer_id, c.balance, is_withdraw ? "WITHDRAW" : "DEPOSIT");
    if (prompt_client(client_fd, prompt_msg, amt_str, sizeof(amt_str)) < 0) {
        unlock_record_data(fd_cust, slot);
        return;
    }

    double amt = atof(amt_str);
    if (amt <= 0.0 || (is_withdraw && amt > c.balance)) {
        unlock_record_data(fd_cust, slot);
        send_display(client_fd, (amt <= 0.0) ? "[CANCELLED] Amount must be positive.\n"
                                             : "[FAILED] Insufficient balance! Write lock released.\n");
        return;
    }

    c.balance += is_withdraw ? -amt : amt;
    write_customer_at(fd_cust, slot, &c);
    unlock_record_data(fd_cust, slot);

    log_transaction(customer_id, is_withdraw ? "WITHDRAW" : "DEPOSIT", amt, c.balance,
                    is_withdraw ? "Self Cash Withdrawal" : "Self Cash Deposit");

    char out[256];
    snprintf(out, sizeof(out),
             "[SUCCESS] %s Rs. %.2f. New Balance: Rs. %.2f (Time: %s).\n",
             is_withdraw ? "Withdrew" : "Deposited", amt, c.balance, c.timestamp);
    send_display(client_fd, out);
}

/* 4. Transfer Funds (Deadlock-Free Ordered Dual-Record Write Lock F_WRLCK) */
static inline void transfer_funds(int client_fd, int fd_cust, int from_slot, int from_id) {
    Customer self_check;
    if (verify_customer_active(client_fd, fd_cust, from_slot, &self_check) < 0) return;

    char dest_str[MAX_INPUT_LEN], amt_str[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter Destination Customer ID (e.g., 2002): ", dest_str, sizeof(dest_str)) < 0) return;
    int to_id = atoi(dest_str);
    if (to_id == from_id) {
        send_display(client_fd, "[ERROR] Cannot transfer funds to the same account.\n");
        return;
    }

    int to_slot = find_customer_slot(fd_cust, to_id, NULL);
    if (to_slot < 0) {
        send_display(client_fd, "[ERROR] Destination Customer ID not found.\n");
        return;
    }

    if (prompt_client(client_fd, "Enter Amount to TRANSFER (Rs.): ", amt_str, sizeof(amt_str)) < 0) return;
    double amt = atof(amt_str);
    if (amt <= 0.0) {
        send_display(client_fd, "[CANCELLED] Transfer amount must be positive.\n");
        return;
    }

    /* Lock smaller slot index first to prevent circular-wait deadlocks */
    int first_slot  = (from_slot < to_slot) ? from_slot : to_slot;
    int second_slot = (from_slot < to_slot) ? to_slot   : from_slot;

    if (lock_record_data(fd_cust, first_slot, F_WRLCK, client_fd, "Transfer-1") < 0) return;
    if (lock_record_data(fd_cust, second_slot, F_WRLCK, client_fd, "Transfer-2") < 0) {
        unlock_record_data(fd_cust, first_slot);
        return;
    }

    Customer c_from, c_to;
    read_customer_at(fd_cust, from_slot, &c_from);
    read_customer_at(fd_cust, to_slot,   &c_to);

    if (strcmp(c_from.status, "ACTIVE") != 0 || strcmp(c_to.status, "ACTIVE") != 0 || amt > c_from.balance) {
        unlock_record_data(fd_cust, second_slot);
        unlock_record_data(fd_cust, first_slot);
        send_display(client_fd, (amt > c_from.balance) ? "[FAILED] Insufficient balance!\n"
                                                       : "[REJECTED] Both accounts must be ACTIVE.\n");
        return;
    }

    c_from.balance -= amt;
    c_to.balance   += amt;
    write_customer_at(fd_cust, from_slot, &c_from);
    write_customer_at(fd_cust, to_slot,   &c_to);

    unlock_record_data(fd_cust, second_slot);
    unlock_record_data(fd_cust, first_slot);

    char note_out[80], note_in[80], msg[384];
    snprintf(note_out, sizeof(note_out), "Transfer to Customer #%06d (%s)", to_id, c_to.firstName);
    snprintf(note_in,  sizeof(note_in),  "Transfer from Customer #%06d (%s)", from_id, c_from.firstName);
    log_transaction(from_id, "TRANSFER_OUT", amt, c_from.balance, note_out);
    log_transaction(to_id,   "TRANSFER_IN",  amt, c_to.balance,   note_in);

    snprintf(msg, sizeof(msg),
             "\n[ACID TRANSFER SUCCESS] Transferred Rs. %.2f to #%06d (%s %s). Your New Balance: Rs. %.2f\n",
             amt, to_id, c_to.firstName, c_to.lastName, c_from.balance);
    send_display(client_fd, msg);
}

/* 5. Apply for a Loan */
static inline void apply_for_loan(int client_fd, int fd_cust, int slot, int customer_id) {
    Customer c;
    if (verify_customer_active(client_fd, fd_cust, slot, &c) < 0) return;

    char amt_str[MAX_INPUT_LEN], purpose[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter Loan Amount Requested (Rs.): ", amt_str, sizeof(amt_str)) < 0) return;
    double amt = atof(amt_str);
    if (amt <= 0.0) {
        send_display(client_fd, "[CANCELLED] Loan amount must be greater than 0.\n");
        return;
    }

    if (prompt_client(client_fd, "Enter Loan Purpose / Remarks: ", purpose, sizeof(purpose)) < 0) return;
    sanitize_csv_field(purpose);
    if (strlen(purpose) == 0) copy_str_safe(purpose, "Personal Loan", sizeof(purpose));

    int fd_loan = open(LOANS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd_loan < 0) return;

    lock_whole_file(fd_loan, F_WRLCK);
    int loan_id = get_next_id(fd_loan, 3000);
    Loan ln = {loan_id, customer_id, 0, amt, "PENDING", "", ""};
    copy_str_safe(ln.remarks, purpose, sizeof(ln.remarks));
    write_loan_at(fd_loan, get_row_count(fd_loan), &ln);
    unlock_whole_file(fd_loan);
    close(fd_loan);

    char msg[320];
    snprintf(msg, sizeof(msg),
             "\n[LOAN APPLICATION SUBMITTED] New Loan ID: %06d | Customer: %06d | Amount: Rs. %.2f | Status: PENDING\n",
             loan_id, customer_id, amt);
    send_display(client_fd, msg);
}

/* 6. Change Customer Password */
static inline void change_customer_password(int client_fd, int fd_cust, int slot) {
    Customer c;
    if (verify_customer_active(client_fd, fd_cust, slot, &c) < 0) return;

    char new_pw[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter New Password (max 15 chars): ", new_pw, sizeof(new_pw)) < 0) return;
    sanitize_csv_field(new_pw);
    if (strlen(new_pw) == 0) {
        send_display(client_fd, "[CANCELLED] Password cannot be empty.\n");
        return;
    }

    if (lock_record_data(fd_cust, slot, F_WRLCK, client_fd, "Customer") < 0) return;
    read_customer_at(fd_cust, slot, &c);
    copy_str_safe(c.password, new_pw, sizeof(c.password));
    write_customer_at(fd_cust, slot, &c);
    unlock_record_data(fd_cust, slot);
    send_display(client_fd, "[SUCCESS] Customer password updated in-place in customers.csv.\n");
}

/* 7. Add Customer Feedback */
static inline void add_feedback(int client_fd, int fd_cust, int slot, int customer_id) {
    Customer c;
    if (verify_customer_active(client_fd, fd_cust, slot, &c) < 0) return;

    char text[MAX_INPUT_LEN];
    if (prompt_client(client_fd, "Enter your feedback message (max 70 chars): ", text, sizeof(text)) < 0) return;
    sanitize_csv_field(text);
    if (strlen(text) == 0) return;

    int fd_fb = open(FEEDBACKS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd_fb < 0) return;

    lock_whole_file(fd_fb, F_WRLCK);
    int fb_id = get_next_id(fd_fb, 4000);
    Feedback fb = {fb_id, customer_id, "", "PENDING", ""};
    copy_str_safe(fb.message, text, sizeof(fb.message));
    write_feedback_at(fd_fb, get_row_count(fd_fb), &fb);
    unlock_whole_file(fd_fb);
    close(fd_fb);

    char msg[200];
    snprintf(msg, sizeof(msg), "[SUCCESS] Feedback #%06d submitted (Status: PENDING).\n", fb_id);
    send_display(client_fd, msg);
}

/* 8. View Transaction History (Passbook + Loan Status) */
static inline void view_transaction_history(int client_fd, int customer_id) {
    char out[MAX_PAYLOAD];
    out[0] = '\0';
    build_passbook_string(customer_id, out, sizeof(out) - 1024);

    int fd_ln = open(LOANS_FILE, O_RDONLY);
    if (fd_ln >= 0) {
        lock_whole_file(fd_ln, F_RDLCK);
        int total = get_row_count(fd_ln), lcount = 0;
        strncat(out, "\n---------------- YOUR LOAN APPLICATIONS (from loans.csv) ----------------\n",
                sizeof(out) - strlen(out) - 1);
        Loan l;
        for (int slot = 1; slot < total; slot++) {
            if (read_loan_at(fd_ln, slot, &l) == 0 && l.userID == customer_id) {
                char line[200];
                snprintf(line, sizeof(line),
                         "Loan #%06d | Amt: Rs. %10.2f | Emp: %06d | Status: %-10s | %s (%s)\n",
                         l.loanID, l.amount, l.assignedEmpID, l.status, l.remarks, l.timestamp);
                if (strlen(out) + strlen(line) + 32 < sizeof(out)) strcat(out, line);
                lcount++;
            }
        }
        if (lcount == 0) strncat(out, "  (No loan applications submitted yet)\n", sizeof(out) - strlen(out) - 1);
        unlock_whole_file(fd_ln);
        close(fd_ln);
    }
    send_display(client_fd, out);
}

/* Customer Session Loop: Returns 0 on Logout, 1 on Exit */
static inline int handle_customer_requests(int client_fd, int fd_cust, int slot, int customer_id) {
    while (1) {
        char menu[768], choice_str[MAX_INPUT_LEN];
        snprintf(menu, sizeof(menu),
                 "\n====================================================================\n"
                 "  CUSTOMER PORTAL [Customer ID: %06d | CSV Slot #%d | Session: ONLINE]\n"
                 "====================================================================\n"
                 "  1. View Account Balance\n"
                 "  2. Deposit Money\n"
                 "  3. Withdraw Money\n"
                 "  4. Transfer Funds\n"
                 "  5. Apply for a Loan\n"
                 "  6. Change Password\n"
                 "  7. Add Feedback\n"
                 "  8. View Transaction History (Passbook)\n"
                 "  9. Logout (Return to Role Selection)\n"
                 " 10. Exit (Disconnect Client)\n"
                 "Enter choice (1-10): ",
                 customer_id, slot);

        if (prompt_client(client_fd, menu, choice_str, sizeof(choice_str)) < 0) return 1;
        switch (atoi(choice_str)) {
            case 1: get_balance(client_fd, fd_cust, slot); break;
            case 2: update_customer_balance(client_fd, fd_cust, slot, customer_id, 0); break;
            case 3: update_customer_balance(client_fd, fd_cust, slot, customer_id, 1); break;
            case 4: transfer_funds(client_fd, fd_cust, slot, customer_id); break;
            case 5: apply_for_loan(client_fd, fd_cust, slot, customer_id); break;
            case 6: change_customer_password(client_fd, fd_cust, slot); break;
            case 7: add_feedback(client_fd, fd_cust, slot, customer_id); break;
            case 8: view_transaction_history(client_fd, customer_id); break;
            case 9:
                send_display(client_fd, "\n[LOGOUT] Logging out of Customer session and releasing session lock...\n");
                return 0;
            case 10: return 1;
            default: send_display(client_fd, "[INVALID] Please enter a valid option (1-10).\n"); break;
        }
    }
}

#endif /* CUSTOMER_H */