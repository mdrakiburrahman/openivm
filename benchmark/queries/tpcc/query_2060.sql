-- {"operators": "EXCEPT_ALL,DISTINCT", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT DISTINCT C_W_ID, C_D_ID, C_ID FROM CUSTOMER EXCEPT ALL SELECT DISTINCT O_W_ID, O_D_ID, O_C_ID FROM OORDER;
