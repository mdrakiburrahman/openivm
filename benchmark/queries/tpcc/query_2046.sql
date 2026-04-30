-- {"operators": "INTERSECT_ALL,INNER_JOIN", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK,ORDER_LINE", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT i.I_ID, s.S_W_ID FROM ITEM i JOIN STOCK s ON i.I_ID = s.S_I_ID INTERSECT ALL SELECT OL_I_ID, OL_SUPPLY_W_ID FROM ORDER_LINE;
