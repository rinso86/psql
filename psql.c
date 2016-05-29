/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author:                                                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_psql.h"
#include "meinincl.h"
#include <pthread.h>

/* If you declare any globals in php_psql.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(psql)
*/

/* True global resources - no need for thread safety here */
static int le_psql;

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("psql.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_psql_globals, psql_globals)
    STD_PHP_INI_ENTRY("psql.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_psql_globals, psql_globals)
PHP_INI_END()
*/
/* }}} */

/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_psql_compiled(string arg)
   Return a string to confirm that the module is compiled in */
PHP_FUNCTION(confirm_psql_compiled)
{
	char *arg = NULL;
	int arg_len, len;
	char *strg;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &arg, &arg_len) == FAILURE) {
		return;
	}

	len = spprintf(&strg, 0, "Congratulations! You have successfully modified ext/%.78s/config.m4. Module %.78s is now compiled into PHP.", "psql", arg);
	RETURN_STRINGL(strg, len, 0);
}
/* }}} */


/* {{{ proto array doqueries(array queries)
   Nimmt einen array von sql-queries und führt diese in parallel aus. */
PHP_FUNCTION(doqueries)
{
	int argc = ZEND_NUM_ARGS();
	zval * queriesdata = NULL;
	zval * dbcreds = NULL;

	if (zend_parse_parameters(argc TSRMLS_CC, "aa", &dbcreds, &queriesdata) == FAILURE) return;

	/* SCHRITT 1: PHP-Input zu c-array
	 In PHP sind alle variablen erst einmal zvals, eine union hinter der mehrere Datentypen liegen koennen.
	 In unserem Fall wissen wir, dass der hinterliegende Datentyp ein HashTable ist.
	 Aus diesem holen wir schliesslich die strings, die in den Hash-vals liegen.
	 Damit haben wir endlich einen blanken c-array von strings bekommen.
	 */
	int laenge_ht = 0;
	char * queries_str[10];
	csv_parameter queries_csv[10];

	HashTable * queriesdata_ht = Z_ARRVAL_P(queriesdata);
	HashPosition position;
	zval ** querydata = NULL;
	for (	zend_hash_internal_pointer_reset_ex(queriesdata_ht, &position);
			zend_hash_get_current_data_ex(queriesdata_ht, (void**) &querydata, &position) == SUCCESS;
			zend_hash_move_forward_ex(queriesdata_ht, &position)) {



		HashTable * querydata_ht = Z_ARRVAL_P(querydata);

		char * query;
		if (zend_has_index_find(querydata_ht, "query", (void **) &query) == FAILURE) { RETURN_NULL();}
		queries_str[laenge_ht] = query;

		HashTable * csvdata_ht;
		if (zend_has_index_find(querydata_ht, "csvdata", (void **) &csvdata_ht) == FAILURE) { RETURN_NULL();}

		int csvcol, discrcol, datecol;
		if (zend_has_index_find(csvdata_ht, "csv", (void **) &csvcol) == FAILURE) { RETURN_NULL();}
		if (zend_has_index_find(csvdata_ht, "discr", (void **) &discrcol) == FAILURE) { RETURN_NULL();}
		if (zend_has_index_find(csvdata_ht, "date", (void **) &datecol) == FAILURE) { RETURN_NULL();}

		csv_parameter csvp = {csvcol, datecol, discrcol};
		queries_csv[laenge_ht] = csvp;

		laenge_ht++;
	}


	HashTable * dbcreds_ht = Z_ARRVAL_P(dbcreds);
	if (zend_has_index_find(queriesdata, "dbcreds", (void **) &dbcreds_ht) == FAILURE) { RETURN_NULL();}

	char * host, usr, pw, db;
	if (zend_has_index_find(dbcreds_ht, "host", (void **) &host) == FAILURE) { RETURN_NULL();}
	if (zend_has_index_find(dbcreds_ht, "usr", (void **) &usr) == FAILURE) { RETURN_NULL();}
	if (zend_has_index_find(dbcreds_ht, "pw", (void **) &pw) == FAILURE) { RETURN_NULL();}
	if (zend_has_index_find(dbcreds_ht, "db", (void **) &db) == FAILURE) { RETURN_NULL();}
	unsigned int port = 0;
	char *socket = NULL;
	unsigned int flags = 0;
	db_creds dbcr = { host, usr, pw, db, port, socket, flags };

	/* SCHRITT 2: Verarbeiten der Daten; c-array zu c-array
	 Hier beginnt die eigentliche Arbeit.
	 */

	/* Eingangsdaten für die Threads. Jeder bekommt selbe DB-Credentials, aber eigenen query.  */
	int num_queries = laenge_ht;
	thread_parameter tps[num_queries];
	int i;
	for (i = 0; i < num_queries; i++) {
		tps[i].query = queries_str[i];
		tps[i].dbcr = &dbcr;
		tps[i].csv = &queries_csv[i];
	}

	if (mysql_library_init(0, NULL, NULL))
		error("Konnte mysql_lib nicht initialisieren.");

	/* Verteilen der Eingangsdaten auf die Threads */
	int num_threads = num_queries;
	pthread_t threads[num_threads];
	int t;
	for (t = 0; t < num_threads; t++) {
		if (pthread_create(&threads[t], NULL, do_query, (void*) &tps[t]) == -1)
			error("Thread nicht erstellt");
	}

	void * result;
	char **** all_data[num_threads];

	/* Hole die Ergebnisse der Threads. pthread_join liefert uns nur einen Pointer zum Ergebnis.
	 Innerhalb des threads haben wir aber glücklicherweise die Werte des Ergebnisses auf dem Heap gespeichert.
	 Vergiss nur nicht, den Heap nach Bearbeitung der Ergebnisse auch wieder frei zu machen.
	 */
	for (t = 0; t < num_threads; t++) {
		/* Here, (void*)thread_out is being saved to void ** result.  */
		if (pthread_join(threads[t], &result) == -1) error("Thread nicht zusammengefügt");
		all_data[t] = (char ****) result;
	}

	mysql_library_end();

//	for (t = 0; t < num_threads; t++) {
//		printNullTerm3DCmtrx(all_data[t]);
//	}



	/*SCHRITT 3: c-array zu PHP-Output
	 Wir bereiten schon mal den return value vor
	 Was hier passiert ist folgendes:
	 Der zval "return_value" bekommt den zu grunde liegenden Datentyp "HashTable".
	 Und dieser wiederum wird befuellt mit den Ergebnissen, die wir aus unserem Modul geholt haben.
	 */
	array_init(return_value);
	for(i=0; i<num_queries; i++){

		char **** query_result = all_data[i];
		zval * query_result_zv;
		int r = 0;
		int rr = 0;
		while(query_result[r] != NULL){

			int d = 0;
			while(query_result[r][d] != NULL){

				zval * row_result_zv;
				int c = 0;
				while(query_result[r][d][c] != NULL){
					add_next_index_string(row_result_zv, query_result[r][d][c], 1);
					c++;
				}

				add_next_index_zval(query_result_zv, row_result_zv);
				rr++;
				d++;
			}
			r++;
		}

		add_assoc_zval(return_value, i, query_result_zv);
	}

	/* Jeder thread hat sein Ergebnis auf dem Heap gespeichert, damit es nach Ende des threads nicht
	 verloren geht. Jetzt müssen wir also den Heap wieder frei machen.
	 */
	for (t = 0; t < num_threads; t++) {
		freeNullTerm3DCmtrx(all_data[t]);
	}

	/* Hier kein "RETURN_TRUE;", verursacht memleak!
	 Vermute, dass return true die weitere Ausfuehrung der zend-engine abbricht,
	 die ansonsten noch den memory fuer "return_value" freigemacht haette.
	 */
}
/* }}} */


/* {{{ php_psql_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_psql_init_globals(zend_psql_globals *psql_globals)
{
	psql_globals->global_value = 0;
	psql_globals->global_string = NULL;
}
*/
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(psql)
{
	/* If you have INI entries, uncomment these lines 
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(psql)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(psql)
{
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(psql)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(psql)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "psql support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/* {{{ psql_functions[]
 *
 * Every user visible function must have an entry in psql_functions[].
 */
const zend_function_entry psql_functions[] = {
	PHP_FE(confirm_psql_compiled,	NULL)		/* For testing, remove later. */
	PHP_FE(doqueries,	NULL)
	PHP_FE_END	/* Must be the last line in psql_functions[] */
};
/* }}} */

/* {{{ psql_module_entry
 */
zend_module_entry psql_module_entry = {
	STANDARD_MODULE_HEADER,
	"psql",
	psql_functions,
	PHP_MINIT(psql),
	PHP_MSHUTDOWN(psql),
	PHP_RINIT(psql),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(psql),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(psql),
	PHP_PSQL_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PSQL
ZEND_GET_MODULE(psql)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
