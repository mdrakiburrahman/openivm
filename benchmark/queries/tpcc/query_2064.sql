-- {"operators": "EXCEPT_ALL,SUBQUERY", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER,HISTORY", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT C_W_ID, C_D_ID FROM CUSTOMER WHERE EXISTS (SELECT 1 FROM OORDER o WHERE o.O_W_ID = C_W_ID AND o.O_D_ID = C_D_ID) EXCEPT ALL SELECT H_C_W_ID, H_C_D_ID FROM HISTORY;
