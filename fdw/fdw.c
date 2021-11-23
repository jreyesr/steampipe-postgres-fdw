// Generated by cgo from fdw.go. Included here so our functions are
// defined and available.
#include "steampipe_postgres_fdw.h"
#include "nodes/plannodes.h"
#include "access/xact.h"

extern PGDLLEXPORT void _PG_init(void);

static int deparseLimit(PlannerInfo *root);
static char *datumToString(Datum datum, Oid type);
static char *convertUUID(char *uuid);

static void fdwGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void fdwGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
static void pgfdw_xact_callback(XactEvent event, void *arg);
static void exitHook(int code, Datum arg);
static ForeignScan *fdwGetForeignPlan(
    PlannerInfo *root,
    RelOptInfo *baserel,
    Oid foreigntableid,
    ForeignPath *best_path,
    List *tlist,
    List *scan_clauses,
    Plan *outer_plan
);

void *serializePlanState(FdwPlanState *state);
FdwExecState *initializeExecState(void *internalstate);
// Required by postgres, doing basic checks to ensure compatibility,
// such as being compiled against the correct major version.
PG_MODULE_MAGIC;

// Define our handling functions with Postgres, following the V1 protocol.
PG_FUNCTION_INFO_V1(fdw_handler);
PG_FUNCTION_INFO_V1(fdw_validator);


/*
 * _PG_init
 * 		Library load-time initalization.
 * 		Sets exitHook() callback for backend shutdown.
 */
void
_PG_init(void)
{
	/* register an exit hook */
	on_proc_exit(&exitHook, PointerGetDatum(NULL));
	RegisterXactCallback(pgfdw_xact_callback, NULL);
}

/*
 * pgfdw_xact_callback gets called when a running
 * query is cancelled
 */
static void
pgfdw_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_ABORT)
	{
		goFdwAbortCallback();
	}
}

/*
 * exitHook
 * 		Close all Oracle connections on process exit.
 */

void
exitHook(int code, Datum arg)
{
	goFdwShutdown();
}

Datum fdw_handler(PG_FUNCTION_ARGS) {
  FdwRoutine *fdw_routine = makeNode(FdwRoutine);
  fdw_routine->GetForeignRelSize = fdwGetForeignRelSize;
  fdw_routine->GetForeignPaths = fdwGetForeignPaths;
  fdw_routine->GetForeignPlan = fdwGetForeignPlan;
  fdw_routine->ExplainForeignScan = goFdwExplainForeignScan;
  fdw_routine->BeginForeignScan = goFdwBeginForeignScan;
  fdw_routine->IterateForeignScan = goFdwIterateForeignScan;
  fdw_routine->ReScanForeignScan = goFdwReScanForeignScan;
  fdw_routine->EndForeignScan = goFdwEndForeignScan;
  fdw_routine->ImportForeignSchema = goFdwImportForeignSchema;
  fdw_routine->ExecForeignInsert = goFdwExecForeignInsert;

PG_RETURN_POINTER(fdw_routine);
}

// TODO - Use this to validate the arguments passed to the FDW
// https://github.com/laurenz/oracle_fdw/blob/9d7b5c331b0c8851c71f410f77b41c1a83c89ece/oracle_fdw.c#L420
Datum fdw_validator(PG_FUNCTION_ARGS) {
  Oid catalog = PG_GETARG_OID(1);
  List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
  goFdwValidate(catalog, options_list);
  PG_RETURN_VOID();
}

static void fdwGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid) {
    // initialise logging`
    // to set the log level for fdw logging from C code, set log_min_messages in postgresql.conf
    goInit();

    FdwPlanState *planstate = palloc0(sizeof(FdwPlanState));
	ForeignTable *ftable = GetForeignTable(foreigntableid);

	ListCell *lc;
	bool needWholeRow = false;
	TupleDesc	desc;

    // Save plan state information
	baserel->fdw_private = planstate;
	planstate->foreigntableid = foreigntableid;

	// Initialize the conversion info array
	{
		Relation	rel = RelationIdGetRelation(ftable->relid);
		AttInMetadata *attinmeta;
		desc = RelationGetDescr(rel);
		attinmeta = TupleDescGetAttInMetadata(desc);
		planstate->numattrs = RelationGetNumberOfAttributes(rel);
		planstate->cinfos = palloc0(sizeof(ConversionInfo *) * planstate->numattrs);
		initConversioninfo(planstate->cinfos, attinmeta);
		needWholeRow = rel->trigdesc && rel->trigdesc->trig_insert_after_row;
		RelationClose(rel);
	}

    // Gather the target_list of columns for this query as Value objects.
	if (needWholeRow) {
		int	i;
		for (i = 0; i < desc->natts; i++) {
			Form_pg_attribute att = TupleDescAttr(desc, i);
			if (!att->attisdropped) {
				planstate->target_list = lappend(planstate->target_list, makeString(NameStr(att->attname)));
			}
		}
	}
	else {
		foreach(lc, extractColumns(baserel->reltarget->exprs, baserel->baserestrictinfo)) {
			Var	  *var = (Var *) lfirst(lc);
			Value	*colname;
			// Store only a Value node containing the string name of the column.
			colname = colnameFromVar(var, root, planstate);
			if (colname != NULL && strVal(colname) != NULL) {
				planstate->target_list = lappend(planstate->target_list, colname);
			}
		}
	}

    // Deduce the limit, if one was specified
    planstate->limit = deparseLimit(root);

    // Inject the "rows" and "width" attribute into the baserel
	goFdwGetRelSize(planstate, root, &baserel->rows, &baserel->reltarget->width, baserel);

	planstate->width = baserel->reltarget->width;
}


/*
 * deparseLimit
 * 		Deparse LIMIT clause to extract the limit count (limit+offset)
 */
static int deparseLimit(PlannerInfo *root)
{
	int limitVal = 0, offsetVal = 0;


	/* don't push down LIMIT if the query has a GROUP BY, DISTINCT, ORDER BY clause or aggregates
	   or if the query refers to more than 1 table */
	if (root->parse->groupClause != NULL ||
	    root->parse->sortClause != NULL ||
	    root->parse->distinctClause != NULL ||
	    root->parse->hasAggs ||
	    root->parse->hasDistinctOn ||
	    bms_num_members(root->all_baserels) != 1
	    )
		return -1;

	/* only push down constant LIMITs that are not NULL */
	if (root->parse->limitCount != NULL && IsA(root->parse->limitCount, Const))
	{
		Const *limit = (Const *)root->parse->limitCount;
		if (limit->constisnull)
			return -1;
		limitVal = atoi(datumToString(limit->constvalue, limit->consttype));
	}
	else{
		return -1;
    }

	/* only consider OFFSETS that are non-NULL constants */
	if (root->parse->limitOffset != NULL && IsA(root->parse->limitOffset, Const))
	{
		Const *offset = (Const *)root->parse->limitOffset;
		if (! offset->constisnull)
			offsetVal = atoi(datumToString(offset->constvalue, offset->consttype));
	}

	return limitVal+offsetVal;
}

/*
 * fdwGetForeignPaths
 *		Create possible access paths for a scan on the foreign table.
 *		This is done by calling the "get_path_keys method on the python side,
 *		and parsing its result to build parameterized paths according to the
 *		equivalence classes found in the plan.
 */
static void fdwGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid) {
	List *paths; /* List of ForeignPath */
	FdwPlanState *planstate = baserel->fdw_private;
	ListCell *lc;
    /* These lists are used to handle sort pushdown */
	List *apply_pathkeys = NULL;
	List *deparsed_pathkeys = NULL;

	/* Extract a friendly version of the pathkeys. */
	List *possiblePaths = goFdwGetPathKeys(planstate);

	/* Try to find parameterized paths */
	paths = findPaths(root, baserel, possiblePaths, planstate->startupCost, planstate, apply_pathkeys, deparsed_pathkeys);

	/* Add a simple default path */
	paths = lappend(paths, create_foreignscan_path(
    root,
    baserel,
		NULL,  /* default pathtarget */
		baserel->rows,
		planstate->startupCost,
		baserel->rows * baserel->reltarget->width * 100000, // table scan is very expensive
		NIL,		/* no pathkeys */
		NULL,
		NULL,
		NULL)
  );

	/* Handle sort pushdown */
	if (root->query_pathkeys) {
		List *deparsed = deparse_sortgroup(root, foreigntableid, baserel);
		if (deparsed) {
			/* Update the sort_*_pathkeys lists if needed */
			computeDeparsedSortGroup(deparsed, planstate, &apply_pathkeys, &deparsed_pathkeys);
		}
	}

	/* Add each ForeignPath previously found */
	foreach(lc, paths) {
		ForeignPath *path = (ForeignPath *) lfirst(lc);
		/* Add the path without modification */
		add_path(baserel, (Path *) path);
		/* Add the path with sort pushdown if possible */
		if (apply_pathkeys && deparsed_pathkeys) {
			ForeignPath *newpath;
			newpath = create_foreignscan_path(
                root,
                baserel,
                NULL,  /* default pathtarget */
                path->path.rows,
                path->path.startup_cost, path->path.total_cost,
                apply_pathkeys, NULL,
                NULL,
                (void *) deparsed_pathkeys
             );
			newpath->path.param_info = path->path.param_info;
			add_path(baserel, (Path *) newpath);
		}
	}
}

/*
 * fdwGetForeignPlan
 *		Create a ForeignScan plan node for scanning the foreign table
 */
static ForeignScan *fdwGetForeignPlan(
  PlannerInfo *root,
  RelOptInfo *baserel,
	Oid foreigntableid,
	ForeignPath *best_path,
	List *tlist,
	List *scan_clauses,
	Plan *outer_plan
) {
    Index scan_relid = baserel->relid;
	FdwPlanState *planstate = (FdwPlanState *) baserel->fdw_private;
	best_path->path.pathtarget->width = planstate->width;
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	planstate->pathkeys = (List *) best_path->fdw_private;
	ForeignScan * s = make_foreignscan(
        tlist,
        scan_clauses,
        scan_relid,
        scan_clauses,		/* no expressions to evaluate */
        serializePlanState(planstate),
        NULL,
        NULL, /* All quals are meant to be rechecked */
        NULL
    );
	return s;
}

/*
 *	"Serialize" a FdwPlanState, so that it is safe to be carried
 *	between the plan and the execution safe.
 */
 void *serializePlanState(FdwPlanState * state) {
 	List *result = NULL;
 	result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->numattrs), false, true));
 	result = lappend(result, state->target_list);
 	result = lappend(result, serializeDeparsedSortGroup(state->pathkeys));
 	result = lappend(result, makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(state->limit), false, true));

 	return result;
 }

/*
 *	"Deserialize" an internal state and inject it in an
 *	FdwExecState
 */
FdwExecState *initializeExecState(void *internalstate) {
    FdwExecState *execstate = palloc0(sizeof(FdwExecState));
	// internalstate is actually a list generated by serializePlanState consisting of:
    //  numattrs, target_list, target_list, pathkeys
	List	   *values = (List *) internalstate;
	AttrNumber	numattrs = ((Const *) linitial(values))->constvalue;
	List		*pathkeys;
	int limit;
	/* Those list must be copied, because their memory context can become */
	/* invalid during the execution (in particular with the cursor interface) */
	execstate->target_list = copyObject(lsecond(values));
	pathkeys = lthird(values);
	limit = ((Const *) lfourth(values))->constvalue;

	execstate->pathkeys = deserializeDeparsedSortGroup(pathkeys);
	execstate->buffer = makeStringInfo();
	execstate->cinfos = palloc0(sizeof(ConversionInfo *) * numattrs);
	execstate->numattrs = numattrs;
	execstate->values = palloc(numattrs * sizeof(Datum));
	execstate->nulls = palloc(numattrs * sizeof(bool));
	execstate->limit = limit;
	return execstate;
}

/*
 * datumToString
 * 		Convert a Datum to a string by calling the type output function.
 * 		Returns the result or NULL if it cannot be converted.
 */
static char
*datumToString(Datum datum, Oid type)
{
	StringInfoData result;
	regproc typoutput;
	HeapTuple tuple;
	char *str, *p;

	/* get the type's output function */
	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type));
	if (!HeapTupleIsValid(tuple))
	{
		elog(ERROR, "cache lookup failed for type %u", type);
	}
	typoutput = ((Form_pg_type)GETSTRUCT(tuple))->typoutput;
	ReleaseSysCache(tuple);

	switch (type)
	{
		case TEXTOID:
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
		case NAMEOID:
		case UUIDOID:
			str = DatumGetCString(OidFunctionCall1(typoutput, datum));

			if (str[0] == '\0')
				return NULL;

			/* strip "-" from "uuid" values */
			if (type == UUIDOID)
				convertUUID(str);

			/* quote string */
			initStringInfo(&result);
			appendStringInfo(&result, "'");
			for (p=str; *p; ++p)
			{
				if (*p == '\'')
					appendStringInfo(&result, "'");
				appendStringInfo(&result, "%c", *p);
			}
			appendStringInfo(&result, "'");
			break;
		case INT8OID:
		case INT2OID:
		case INT4OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			str = DatumGetCString(OidFunctionCall1(typoutput, datum));
			initStringInfo(&result);
			appendStringInfo(&result, "%s", str);
			break;
//        case DATEOID:
//			str = deparseDate(datum);
//			initStringInfo(&result);
//			appendStringInfo(&result, "(CAST ('%s' AS DATE))", str);
//			break;
//		case TIMESTAMPOID:
//			str = deparseTimestamp(datum, false);
//			initStringInfo(&result);
//			appendStringInfo(&result, "(CAST ('%s' AS TIMESTAMP))", str);
//			break;
//		case TIMESTAMPTZOID:
//			str = deparseTimestamp(datum, true);
//			initStringInfo(&result);
//			appendStringInfo(&result, "(CAST ('%s' AS TIMESTAMP WITH TIME ZONE))", str);
//			break;
//		case INTERVALOID:
//			str = deparseInterval(datum);
//			if (str == NULL)
//				return NULL;
//			initStringInfo(&result);
//			appendStringInfo(&result, "%s", str);
//			break;
		default:
			return NULL;
	}

	return result.data;
}
/*
 * convertUUID
 * 		Strip "-" from a PostgreSQL "uuid" so that Oracle can parse it.
 * 		In addition, convert the string to upper case.
 * 		This modifies the argument in place!
 */
char
*convertUUID(char *uuid)
{
	char *p = uuid, *q = uuid, c;

	while (*p != '\0')
	{
		if (*p == '-')
			++p;
		c = *(p++);
		if (c >= 'a' && c <= 'f')
			*(q++) = c - ('a' - 'A');
		else
			*(q++) = c;
	}
	*q = '\0';

	return uuid;
}