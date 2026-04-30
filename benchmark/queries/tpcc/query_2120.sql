-- {"operators": "SAMPLE,INTERSECT_ALL", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "op:SAMPLE"}
SELECT S_I_ID FROM STOCK USING SAMPLE reservoir(20 ROWS) REPEATABLE (23) INTERSECT ALL SELECT OL_I_ID FROM ORDER_LINE;
