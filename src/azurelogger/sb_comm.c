int createobdinsertstmt(sqlite3 *db,sqlite3_stmt **ret_stmt, void *obdcaps) {
		// TODO calculate buffer size and create correct sized one,
		//   otherwise this could overflow if obdservicecommands contains a *lot* of non-NULL fields
	int i;

	int columncount = 0;
	char insert_sql[4096] = "INSERT INTO obd (";
	for(i=0; i<sizeof(obdcmds_mode1)/sizeof(obdcmds_mode1[0]); i++) {
		if(NULL != obdcmds_mode1[i].db_column  && isobdcapabilitysupported(obdcaps,i)) {
			strcat(insert_sql,obdcmds_mode1[i].db_column);
			strcat(insert_sql,",");
			columncount++;
		}
	}
	strcat(insert_sql,"time,trip) VALUES (");
	for(i=0; i<columncount; i++) {
		strcat(insert_sql,"?,");
	}
	strcat(insert_sql,"?,?)");

	columncount++; // for time
	// printf("insert_sql:\n  %s\n", insert_sql);

	int rc;
	const char *zTail;
	rc = sqlite3_prepare_v2(db,insert_sql,-1,ret_stmt,&zTail);
	if(SQLITE_OK != rc) {
		fprintf(stderr, "Can't prepare statement %s: %s\n", insert_sql, sqlite3_errmsg(db));
		return 0;
	}

	return columncount;
}
