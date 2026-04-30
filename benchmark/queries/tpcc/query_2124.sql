-- {"operators": "POSITIONAL_JOIN,WINDOW", "complexity": "high", "is_incremental": false, "has_nulls": false, "has_cast": false, "has_case": false, "tables": "CUSTOMER,OORDER", "non_incr_reason": "op:POSITIONAL_JOIN"}
SELECT c.C_W_ID, c.C_ID, o.O_ID, ROW_NUMBER() OVER (ORDER BY c.C_W_ID, c.C_ID) AS pos_rank FROM CUSTOMER c POSITIONAL JOIN OORDER o;
