-- {"operators": "INTERSECT_ALL", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,HISTORY", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT C_W_ID AS w_id, C_D_ID AS d_id FROM CUSTOMER INTERSECT ALL SELECT H_C_W_ID AS w_id, H_C_D_ID AS d_id FROM HISTORY;
