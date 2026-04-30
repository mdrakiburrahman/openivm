-- {"operators": "INTERSECT_ALL,SUBQUERY", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER,HISTORY", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT C_W_ID, C_D_ID FROM CUSTOMER WHERE C_ID IN (SELECT O_C_ID FROM OORDER) INTERSECT ALL SELECT H_C_W_ID, H_C_D_ID FROM HISTORY;
