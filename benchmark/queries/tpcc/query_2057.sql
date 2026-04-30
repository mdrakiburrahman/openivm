-- {"operators": "EXCEPT_ALL,CTE", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "DISTRICT,OORDER", "non_incr_reason": "kw:EXCEPT_ALL"}
WITH district_keys AS (SELECT D_W_ID, D_ID FROM DISTRICT), order_keys AS (SELECT O_W_ID, O_D_ID FROM OORDER) SELECT * FROM district_keys EXCEPT ALL SELECT * FROM order_keys;
