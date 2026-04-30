-- {"operators": "EXCEPT_ALL,LEFT_JOIN", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK,ORDER_LINE", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT i.I_ID, s.S_W_ID FROM ITEM i LEFT JOIN STOCK s ON i.I_ID = s.S_I_ID EXCEPT ALL SELECT OL_I_ID, OL_SUPPLY_W_ID FROM ORDER_LINE;
