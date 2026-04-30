-- {"operators": "FILTER,SEMI_JOIN,IN", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER", "non_incr_reason": "join:SEMI"}
SELECT c.C_W_ID, c.C_D_ID, c.C_ID FROM CUSTOMER c WHERE (c.C_W_ID, c.C_D_ID, c.C_ID) IN (SELECT O_W_ID, O_D_ID, O_C_ID FROM OORDER);
