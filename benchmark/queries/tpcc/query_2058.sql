-- {"operators": "EXCEPT_ALL,INNER_JOIN", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "WAREHOUSE,DISTRICT,HISTORY", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT w.W_ID, d.D_ID FROM WAREHOUSE w JOIN DISTRICT d ON w.W_ID = d.D_W_ID EXCEPT ALL SELECT H_W_ID, H_D_ID FROM HISTORY;
