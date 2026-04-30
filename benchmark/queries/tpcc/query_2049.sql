-- {"operators": "INTERSECT_ALL,UNION", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "WAREHOUSE,DISTRICT,HISTORY", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT W_ID AS w_id FROM WAREHOUSE UNION ALL SELECT D_W_ID FROM DISTRICT INTERSECT ALL SELECT H_W_ID FROM HISTORY;
