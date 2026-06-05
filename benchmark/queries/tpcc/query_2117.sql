-- {"operators": "SAMPLE,INNER_JOIN", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK"}
SELECT i.I_ID, i.I_NAME, s.S_W_ID, s.S_QUANTITY FROM (SELECT * FROM STOCK USING SAMPLE reservoir(20 ROWS) REPEATABLE (13)) s JOIN ITEM i ON s.S_I_ID = i.I_ID;
