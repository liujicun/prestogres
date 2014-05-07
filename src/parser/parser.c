/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * analyze.c and related files.
 *
 *
 * Portions Copyright (c) 2003-2009, PgPool Global Development Group
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/parser/parser.c
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>
#include "pool_parser.h"
#include "utils/palloc.h"
#include "gramparse.h"	/* required before parser/gram.h! */
#include "gram.h"
#include "parser.h"
#include "pg_wchar.h"
#include "utils/elog.h"

List	   *parsetree;			/* result of parsing is left here */
int			server_version_num = 0;

static pg_enc		server_encoding = PG_SQL_ASCII;
static	bool		in_parser_context = false;

static int
parse_version(const char *versionString);

/*
 * raw_parser
 *		Given a query in string form, do lexical and grammatical analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.
 */
List *
raw_parser(const char *str)
{
	core_yyscan_t yyscanner;
	base_yy_extra_type yyextra;
	int			yyresult;

//	Do we need a seperate memory context here?
//	if (pool_memory == NULL)
//		pool_memory = pool_memory_create(PARSER_BLOCK_SIZE);

	parsetree = NIL;			/* in case grammar forgets to set it */

	/* initialize the flex scanner */
	yyscanner = scanner_init(str, &yyextra.core_yy_extra,
							 ScanKeywords, NumScanKeywords);

	/* base_yylex() only needs this much initialization */
	yyextra.have_lookahead = false;

	/* initialize the bison parser */
	parser_init(&yyextra);
	in_parser_context = true;
	PG_TRY();
	{
		yyresult = base_yyparse(yyscanner);
		scanner_finish(yyscanner);
		in_parser_context = false;
		if (yyresult)				/* error */
			return NIL;
		return yyextra.parsetree;
	}
	PG_CATCH();
	{
		scanner_finish(yyscanner);
		in_parser_context = false;
		return NIL; /* error */
	}
	PG_END_TRY();
	return yyextra.parsetree;
}

void free_parser(void)
{
	//pool_memory_delete(pool_memory, 1);
}

/*
 * Intermediate filter between parser and base lexer (base_yylex in scan.l).
 *
 * The filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.	We reduce these cases to one-token
 * lookahead by combining tokens here, in order to keep the grammar LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do it without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 *
 * The filter also provides a convenient place to translate between
 * the core_YYSTYPE and YYSTYPE representations (which are really the
 * same thing anyway, but notationally they're different).
 */
int
base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner)
{
	base_yy_extra_type *yyextra = pg_yyget_extra(yyscanner);
	int			cur_token;
	int			next_token;
	core_YYSTYPE cur_yylval;
	YYLTYPE		cur_yylloc;

	/* Get next token --- we might already have it */
	if (yyextra->have_lookahead)
	{
		cur_token = yyextra->lookahead_token;
		lvalp->core_yystype = yyextra->lookahead_yylval;
		*llocp = yyextra->lookahead_yylloc;
		yyextra->have_lookahead = false;
	}
	else
		cur_token = core_yylex(&(lvalp->core_yystype), llocp, yyscanner);

	/* Do we need to look ahead for a possible multiword token? */
	switch (cur_token)
	{
		case NULLS_P:

			/*
			 * NULLS FIRST and NULLS LAST must be reduced to one token
			 */
			cur_yylval = lvalp->core_yystype;
			cur_yylloc = *llocp;
			next_token = core_yylex(&(lvalp->core_yystype), llocp, yyscanner);
			switch (next_token)
			{
				case FIRST_P:
					cur_token = NULLS_FIRST;
					break;
				case LAST_P:
					cur_token = NULLS_LAST;
					break;
				default:
					/* save the lookahead token for next time */
					yyextra->lookahead_token = next_token;
					yyextra->lookahead_yylval = lvalp->core_yystype;
					yyextra->lookahead_yylloc = *llocp;
					yyextra->have_lookahead = true;
					/* and back up the output info to cur_token */
					lvalp->core_yystype = cur_yylval;
					*llocp = cur_yylloc;
					break;
			}
			break;

		case WITH:

			/*
			 * WITH TIME must be reduced to one token
			 */
			cur_yylval = lvalp->core_yystype;
			cur_yylloc = *llocp;
			next_token = core_yylex(&(lvalp->core_yystype), llocp, yyscanner);
			switch (next_token)
			{
				case TIME:
					cur_token = WITH_TIME;
					break;
				default:
					/* save the lookahead token for next time */
					yyextra->lookahead_token = next_token;
					yyextra->lookahead_yylval = lvalp->core_yystype;
					yyextra->lookahead_yylloc = *llocp;
					yyextra->have_lookahead = true;
					/* and back up the output info to cur_token */
					lvalp->core_yystype = cur_yylval;
					*llocp = cur_yylloc;
					break;
			}
			break;

		default:
			break;
	}

	return cur_token;
}


static int
parse_version(const char *versionString)
{
	int			cnt;
	int			vmaj,
				vmin,
				vrev;

	cnt = sscanf(versionString, "%d.%d.%d", &vmaj, &vmin, &vrev);

	if (cnt < 2)
		return -1;

	if (cnt == 2)
		vrev = 0;

	return (100 * vmaj + vmin) * 100 + vrev;
}

void
parser_set_param(const char *name, const char *value)
{
	if (strcmp(name, "server_version") == 0)
	{
		server_version_num = parse_version(value);
	}
	else if (strcmp(name, "server_encoding") == 0)
	{
		if (strcmp(value, "UTF8") == 0)
			server_encoding = PG_UTF8;
		else
			server_encoding = PG_SQL_ASCII;
	}
	else if (strcmp(name, "standard_conforming_strings") == 0)
	{
		if (strcmp(value, "on") == 0)
			standard_conforming_strings = true;
		else
			standard_conforming_strings = false;
	}
}

int
GetDatabaseEncoding(void)
{
	return server_encoding;
}

int
pg_mblen(const char *mbstr)
{
	return pg_utf_mblen((const unsigned char *) mbstr);
}
