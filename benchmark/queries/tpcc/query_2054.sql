-- {"operators": "EXCEPT_ALL", "complexity": "medium", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK", "non_incr_reason": "kw:EXCEPT_ALL"}
SELECT I_ID FROM ITEM EXCEPT ALL SELECT S_I_ID FROM STOCK;
