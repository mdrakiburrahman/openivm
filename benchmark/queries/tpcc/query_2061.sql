-- {"operators": "EXCEPT_ALL,UNION", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ORDER_LINE,ITEM", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT S_I_ID AS item_id FROM STOCK UNION ALL SELECT OL_I_ID FROM ORDER_LINE EXCEPT ALL SELECT I_ID FROM ITEM WHERE I_PRICE < 20;
