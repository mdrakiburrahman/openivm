-- {"operators": "EXCEPT_ALL,FILTER", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "STOCK,ORDER_LINE", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT S_W_ID, S_I_ID FROM STOCK WHERE S_QUANTITY < 60 EXCEPT ALL SELECT OL_SUPPLY_W_ID, OL_I_ID FROM ORDER_LINE WHERE OL_QUANTITY > 0;
