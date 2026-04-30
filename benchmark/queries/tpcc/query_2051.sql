-- {"operators": "INTERSECT_ALL,FULL_OUTER_JOIN", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK,ORDER_LINE", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT COALESCE(i.I_ID, s.S_I_ID) AS item_id FROM ITEM i FULL OUTER JOIN STOCK s ON i.I_ID = s.S_I_ID INTERSECT ALL SELECT OL_I_ID FROM ORDER_LINE;
