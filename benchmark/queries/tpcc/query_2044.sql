-- {"operators": "INTERSECT_ALL,FILTER", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,ORDER_LINE", "non_incr_reason": "kw:INTERSECT_ALL"}
SELECT I_ID FROM ITEM WHERE I_PRICE > 20 INTERSECT ALL SELECT OL_I_ID FROM ORDER_LINE WHERE OL_AMOUNT > 0;
