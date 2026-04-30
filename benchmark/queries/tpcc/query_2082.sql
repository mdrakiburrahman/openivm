-- {"operators": "FILTER,ANTI_JOIN,NOT_IN", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER", "non_incr_reason": "join:ANTI"}
SELECT c.C_W_ID, c.C_D_ID, c.C_ID FROM CUSTOMER c WHERE (c.C_W_ID, c.C_D_ID, c.C_ID) NOT IN (SELECT O_W_ID, O_D_ID, O_C_ID FROM OORDER);
