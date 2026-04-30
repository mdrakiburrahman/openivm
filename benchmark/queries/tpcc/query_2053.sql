-- {"operators": "INTERSECT_ALL,CAST", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": true, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT CAST(S_I_ID AS BIGINT) AS item_id FROM STOCK INTERSECT ALL SELECT CAST(OL_I_ID AS BIGINT) FROM ORDER_LINE;
