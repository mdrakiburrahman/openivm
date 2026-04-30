-- {"operators": "FILTER,ANTI_JOIN,SUBQUERY,WINDOW", "complexity": "high", "is_incremental": true, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "ITEM,STOCK"}
SELECT i.I_ID, ROW_NUMBER() OVER (ORDER BY i.I_PRICE DESC) AS price_rank FROM ITEM i WHERE NOT EXISTS (SELECT 1 FROM STOCK s WHERE s.S_I_ID = i.I_ID);
