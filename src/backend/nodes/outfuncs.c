/*
 *
 * outfuncs.c--
 *	  routines to convert a node to ascii representation
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *  $Id: outfuncs.c,v 1.54 1998/12/15 02:32:57 scrappy Exp $
 *
 * NOTES
 *	  Every (plan) node in POSTGRES has an associated "out" routine which
 *	  knows how to create its ascii representation. These functions are
 *	  useful for debugging as well as for storing plans in the system
 *	  catalogs (eg. indexes). This is also the plan string sent out in
 *	  Mariposa.
 *
 *	  These functions update the in/out argument of type StringInfo
 *	  passed to them. This argument contains the string holding the ASCII
 *	  representation plus some other information (string length, etc.)
 *
 */
#include <stdio.h>
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/datum.h"
#include "utils/palloc.h"

#include "nodes/nodes.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"

#include "catalog/pg_type.h"
#include "lib/stringinfo.h"

#ifdef PARSEDEBUG
#include "../parse.h"
#endif

static void _outDatum(StringInfo str, Datum value, Oid type);
static void _outNode(StringInfo str, void *obj);

/*
 * _outIntList -
 *	   converts a List of integers
 */
static void
_outIntList(StringInfo str, List *list)
{
	List	*l;

	appendStringInfo(str, "(");
	foreach(l, list)
	{
		appendStringInfo(str, " %d ", (int) lfirst(l));
	}
	appendStringInfo(str, ")");
}

static void
_outCreateStmt(StringInfo str, CreateStmt *node)
{
	appendStringInfo(str, " CREATE :relname %s :columns ", node->relname);

	_outNode(str, node->tableElts);
	appendStringInfo(str, " :inhRelnames ");
	_outNode(str, node->inhRelnames);
	appendStringInfo(str, " :constraints ");
	_outNode(str, node->constraints);
}

static void
_outIndexStmt(StringInfo str, IndexStmt *node)
{
	appendStringInfo(str, 
			" INDEX :idxname %s :relname %s :accessMethod %s :indexParams ",
			node->idxname, node->relname, node->accessMethod);

	_outNode(str, node->indexParams);
	appendStringInfo(str, " :withClause ");
	_outNode(str, node->withClause);
	appendStringInfo(str, " :whereClause ");
	_outNode(str, node->whereClause);
	appendStringInfo(str, " :rangetable ");
	_outNode(str, node->rangetable);

	appendStringInfo(str, " :lossy %s :unique %s ",
			node->lossy ? "true" : "false",
			node->unique ? "true" : "false");
}

#ifdef PARSEDEBUG
static void
_outSelectStmt(StringInfo str, SelectStmt *node)
{
	appendStringInfo(str, "SELECT :where ");
	_outNode(str, node->whereClause);
}

static void
_outFuncCall(StringInfo str, FuncCall *node)
{
	appendStringInfo(str, "FUNCTION %s :args ", node->funcname);
	_outNode(str, node->args);
}

#endif

static void
_outColumnDef(StringInfo str, ColumnDef *node)
{
	appendStringInfo(str, " COLUMNDEF :colname %s :typename ", node->colname);
	_outNode(str, node->typename);

	appendStringInfo(str, " :is_not_null %s :defval %s :constraints ",
			node->is_not_null ? "true" : "false", node->defval);
	_outNode(str, node->constraints);
}

static void
_outTypeName(StringInfo str, TypeName *node)
{
	appendStringInfo(str, 
			" TYPENAME :name %s :timezone %s :setof %s typmod %d :arrayBounds ",
			node->name, 
			node->timezone ? "true" : "false",
			node->setof ? "true" : "false",
			node->typmod);

	appendStringInfo(str, " :arrayBounds ");
	_outNode(str, node->arrayBounds);
}

static void
_outIndexElem(StringInfo str, IndexElem *node)
{
	appendStringInfo(str, " INDEXELEM :name %s :args ", node->name);
	_outNode(str, node->args);

	appendStringInfo(str, " :class %s :typename ", node->class);
	_outNode(str, node->typename);
}

static void
_outQuery(StringInfo str, Query *node)
{
	appendStringInfo(str, " QUERY :command %d ", node->commandType);

	if (node->utilityStmt)
	{
		switch (nodeTag(node->utilityStmt))
		{
			case T_CreateStmt:
				appendStringInfo(str, " :create %s ",
						((CreateStmt *) (node->utilityStmt))->relname);
				_outNode(str, node->utilityStmt);
				break;

			case T_IndexStmt:
				appendStringInfo(str, " :index %s on %s ",
					((IndexStmt *) (node->utilityStmt))->idxname,
					((IndexStmt *) (node->utilityStmt))->relname);
				_outNode(str, node->utilityStmt);
				break;

			case T_NotifyStmt:
				appendStringInfo(str, " :utility %s ",
						((NotifyStmt *) (node->utilityStmt))->relname);
				break;

			default:
				appendStringInfo(str, " :utility ? ");
		}
	}
	else
	{
		appendStringInfo(str, " :utility %s", NULL);
	}

	appendStringInfo(str, 
			" :resultRelation %d :into %s :isPortal %s :isBinary %s :unionall %s ",
			node->resultRelation,
			node->into,
			node->isPortal ? "true" : "false",
			node->isBinary ? "true" : "false",
			node->unionall ? "true" : "false");

	appendStringInfo(str, " :unique %s :sortClause ", node->uniqueFlag);
	_outNode(str, node->sortClause);

	appendStringInfo(str, " :rtable ");
	_outNode(str, node->rtable);

	appendStringInfo(str, " :targetlist ");
	_outNode(str, node->targetList);

	appendStringInfo(str, " :qual ");
	_outNode(str, node->qual);

	appendStringInfo(str, " :groupClause ");
	_outNode(str, node->groupClause);

	appendStringInfo(str, " :havingQual ");
	_outNode(str, node->havingQual);

	appendStringInfo(str, " :hasAggs %s :hasSubLinks %s :unionClause ",
			node->hasAggs ? "true" : "false",
			node->hasSubLinks ? "true" : "false");
	_outNode(str, node->unionClause);

	appendStringInfo(str, " :limitOffset ");
	_outNode(str, node->limitOffset);

	appendStringInfo(str, " :limitCount ");
	_outNode(str, node->limitCount);
}

static void
_outSortClause(StringInfo str, SortClause *node)
{
	appendStringInfo(str, " SORTCLAUSE :resdom ");
	_outNode(str, node->resdom);

	appendStringInfo(str, " :opoid %u ", node->opoid);
}

static void
_outGroupClause(StringInfo str, GroupClause *node)
{
	appendStringInfo(str, " GROUPCLAUSE :entry ");
	_outNode(str, node->entry);

	appendStringInfo(str, " :grpOpoid %u ", node->grpOpoid);
}

/*
 * print the basic stuff of all nodes that inherit from Plan
 */
static void
_outPlanInfo(StringInfo str, Plan *node)
{
  appendStringInfo(str, 
		":cost %g :size %d :width %d :state %s :qptargetlist ",
		node->cost,
		node->plan_size,
		node->plan_width,
		node->state ? "not-NULL" : "<>");
	_outNode(str, node->targetlist);

	appendStringInfo(str, " :qpqual ");
	_outNode(str, node->qual);

	appendStringInfo(str, " :lefttree ");
	_outNode(str, node->lefttree);

	appendStringInfo(str, " :righttree ");
	_outNode(str, node->righttree);

	appendStringInfo(str, " :extprm ");
	_outIntList(str, node->extParam);

	appendStringInfo(str, " :locprm ");
	_outIntList(str, node->locParam);

	appendStringInfo(str, " :initplan ");
	_outNode(str, node->initPlan);

	appendStringInfo(str, " :nprm %d ", node->nParamExec);
}

/*
 *	Stuff from plannodes.h
 */
static void
_outPlan(StringInfo str, Plan *node)
{
	appendStringInfo(str, " PLAN ");
	_outPlanInfo(str, (Plan *) node);
}

static void
_outResult(StringInfo str, Result *node)
{
	appendStringInfo(str, " RESULT ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :resconstantqual ");
	_outNode(str, node->resconstantqual);

}

/*
 *	Append is a subclass of Plan.
 */
static void
_outAppend(StringInfo str, Append *node)
{
	appendStringInfo(str, " APPEND ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :appendplans ");
	_outNode(str, node->appendplans);

	appendStringInfo(str, " :unionrtables ");
	_outNode(str, node->unionrtables);

	appendStringInfo(str, 
		" :inheritrelid %d :inheritrtable ", 
		node->inheritrelid);
	_outNode(str, node->inheritrtable);

}

/*
 *	Join is a subclass of Plan
 */
static void
_outJoin(StringInfo str, Join *node)
{
	appendStringInfo(str, " JOIN ");
	_outPlanInfo(str, (Plan *) node);

}

/*
 *	NestLoop is a subclass of Join
 */
static void
_outNestLoop(StringInfo str, NestLoop *node)
{
	appendStringInfo(str, " NESTLOOP ");
	_outPlanInfo(str, (Plan *) node);
}

/*
 *	MergeJoin is a subclass of Join
 */
static void
_outMergeJoin(StringInfo str, MergeJoin *node)
{
	appendStringInfo(str, " MERGEJOIN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :mergeclauses ");
	_outNode(str, node->mergeclauses);

	appendStringInfo(str, 
			" :mergejoinop %u :mergerightorder %u :mergeleftorder %u ",
			node->mergejoinop, 
			node->mergerightorder,
			node->mergeleftorder);
}

/*
 *	HashJoin is a subclass of Join.
 */
static void
_outHashJoin(StringInfo str, HashJoin *node)
{
	appendStringInfo(str, " HASHJOIN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :hashclauses ");
	_outNode(str, node->hashclauses);

	appendStringInfo(str, 
			" :hashjoinop %u :hashjointable 0x%x :hashjointablekey %d ",
			node->hashjoinop,
			(int) node->hashjointable,
			node->hashjointablekey);

	appendStringInfo(str, 
			" :hashjointablesize %d :hashdone %d ",
			node->hashjointablesize,
			node->hashdone);
}

static void
_outSubPlan(StringInfo str, SubPlan *node)
{
	appendStringInfo(str, " SUBPLAN :plan ");
	_outNode(str, node->plan);

	appendStringInfo(str, " :planid %u :rtable ", node->plan_id);
	_outNode(str, node->rtable);

	appendStringInfo(str, " :setprm ");
	_outIntList(str, node->setParam);

	appendStringInfo(str, " :parprm ");
	_outIntList(str, node->parParam);

	appendStringInfo(str, " :slink ");
	_outNode(str, node->sublink);
}

/*
 *	Scan is a subclass of Node
 */
static void
_outScan(StringInfo str, Scan *node)
{
	appendStringInfo(str, " SCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %d ", node->scanrelid);
}

/*
 *	SeqScan is a subclass of Scan
 */
static void
_outSeqScan(StringInfo str, SeqScan *node)
{
	appendStringInfo(str, " SEQSCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %d ", node->scanrelid);
}

/*
 *	IndexScan is a subclass of Scan
 */
static void
_outIndexScan(StringInfo str, IndexScan *node)
{
	appendStringInfo(str, " INDEXSCAN ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :scanrelid %d :indxid ", node->scan.scanrelid);
	_outIntList(str, node->indxid);

	appendStringInfo(str, " :indxqual ");
	_outNode(str, node->indxqual);

	appendStringInfo(str, " :indxqualorig ");
	_outNode(str, node->indxqualorig);

}

/*
 *	Temp is a subclass of Plan
 */
static void
_outTemp(StringInfo str, Temp *node)
{
	appendStringInfo(str, " TEMP ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :tempid %u :keycount %d ", 
			node->tempid,
			node->keycount);
}

/*
 *	Sort is a subclass of Temp
 */
static void
_outSort(StringInfo str, Sort *node)
{
	appendStringInfo(str, " SORT ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :tempid %u :keycount %d ",
			node->tempid,
			node->keycount);
}

static void
_outAgg(StringInfo str, Agg *node)
{

	appendStringInfo(str, " AGG ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :aggs ");
	_outNode(str, node->aggs);
}

static void
_outGroup(StringInfo str, Group *node)
{
	appendStringInfo(str, " GRP ");
	_outPlanInfo(str, (Plan *) node);

	/* the actual Group fields */
	appendStringInfo(str, " :numCols %d :tuplePerGroup %s ",
			node->numCols,
			node->tuplePerGroup ? "true" : "false");
}

/*
 *	For some reason, unique is a subclass of Temp.
 */
static void
_outUnique(StringInfo str, Unique *node)
{
	appendStringInfo(str, " UNIQUE ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :tempid %u :keycount %d ",
			node->tempid,
			node->keycount);
}


/*
 *	Hash is a subclass of Temp
 */
static void
_outHash(StringInfo str, Hash *node)
{
	appendStringInfo(str, " HASH ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :hashkey ");
	_outNode(str, node->hashkey);

	appendStringInfo(str, " :hashtable 0x%x :hashtablekey %d :hashtablesize %d ",
			(int) node->hashtable,
			node->hashtablekey,
			node->hashtablesize);
}

static void
_outTee(StringInfo str, Tee *node)
{
	appendStringInfo(str, " TEE ");
	_outPlanInfo(str, (Plan *) node);

	appendStringInfo(str, " :leftParent %X :rightParent %X ",
		(int) node->leftParent,
		(int) node->rightParent);

	appendStringInfo(str, " :rtentries ");
	_outNode(str, node->rtentries);
}

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

/*
 *	Resdom is a subclass of Node
 */
static void
_outResdom(StringInfo str, Resdom *node)
{
	appendStringInfo(str, " RESDOM :resno %d :restype %u :restypmod %d ",
			node->resno,
			node->restype,
			node->restypmod);

	appendStringInfo(str, " :resname \"%s\" :reskey %d :reskeyop %u :resjunk %d",
			node->resname,
			node->reskey,
			node->reskeyop,
			node->resjunk);
}

static void
_outFjoin(StringInfo str, Fjoin *node)
{
	int			i;

	appendStringInfo(str, " FJOIN :initialized %s :nNodes %d ",
			node->fj_initialized ? "true" : "false",
			node->fj_nNodes);

	appendStringInfo(str, " :innerNode ");
	_outNode(str, node->fj_innerNode);

	appendStringInfo(str, " :results @ 0x%x :alwaysdone", 
			(int) node->fj_results);

	for (i = 0; i < node->fj_nNodes; i++)
		appendStringInfo(str, (node->fj_alwaysDone[i]) ? "true" : "false");
}

/*
 *	Expr is a subclass of Node
 */
static void
_outExpr(StringInfo str, Expr *node)
{
	char		buf[500];
	char	   *opstr = NULL;

	appendStringInfo(str, " EXPR ");

	sprintf(buf, " :typeOid %u ", node->typeOid);
	appendStringInfo(str, buf);
	switch (node->opType)
	{
		case OP_EXPR:
			opstr = "op";
			break;
		case FUNC_EXPR:
			opstr = "func";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case AND_EXPR:
			opstr = "and";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
		case SUBPLAN_EXPR:
			opstr = "subp";
			break;
	}
	appendStringInfo(str, " :opType ");
	appendStringInfo(str, opstr);
	appendStringInfo(str, " :oper ");
	_outNode(str, node->oper);
	appendStringInfo(str, " :args ");
	_outNode(str, node->args);
}

/*
 *	Var is a subclass of Expr
 */
static void
_outVar(StringInfo str, Var *node)
{
	char		buf[500];

	appendStringInfo(str, " VAR ");
	sprintf(buf, " :varno %d ", node->varno);
	appendStringInfo(str, buf);
	sprintf(buf, " :varattno %d ", node->varattno);
	appendStringInfo(str, buf);
	sprintf(buf, " :vartype %u ", node->vartype);
	appendStringInfo(str, buf);
	sprintf(buf, " :vartypmod %d ", node->vartypmod);
	appendStringInfo(str, buf);
	sprintf(buf, " :varlevelsup %u ", node->varlevelsup);
	appendStringInfo(str, buf);
	sprintf(buf, " :varnoold %d ", node->varnoold);
	appendStringInfo(str, buf);
	sprintf(buf, " :varoattno %d ", node->varoattno);
	appendStringInfo(str, buf);
}

/*
 *	Const is a subclass of Expr
 */
static void
_outConst(StringInfo str, Const *node)
{
	char		buf[500];

	appendStringInfo(str, " CONST ");
	sprintf(buf, " :consttype %u ", node->consttype);
	appendStringInfo(str, buf);
	sprintf(buf, " :constlen %d ", node->constlen);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :constisnull ");
	appendStringInfo(str, node->constisnull ? "true" : "false");
	appendStringInfo(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfo(str, "<>");
	else
		_outDatum(str, node->constvalue, node->consttype);
	appendStringInfo(str, " :constbyval ");
	appendStringInfo(str, node->constbyval ? "true" : "false");
}

/*
 *	Aggreg
 */
static void
_outAggreg(StringInfo str, Aggreg *node)
{
	char		buf[500];

	appendStringInfo(str, " AGGREG ");
	appendStringInfo(str, " :aggname ");
	appendStringInfo(str, (char *) node->aggname);
	sprintf(buf, " :basetype %u ", node->basetype);
	appendStringInfo(str, buf);
	sprintf(buf, " :aggtype %u ", node->aggtype);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :target ");
	_outNode(str, node->target);
	sprintf(buf, " :aggno %d ", node->aggno);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :usenulls ");
	appendStringInfo(str, node->usenulls ? "true" : "false");
}

/*
 *	SubLink
 */
static void
_outSubLink(StringInfo str, SubLink *node)
{
	char		buf[500];

	appendStringInfo(str, " SUBLINK ");
	sprintf(buf, " :subLinkType %d ", node->subLinkType);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :useor ");
	appendStringInfo(str, node->useor ? "true" : "false");
	appendStringInfo(str, " :lefthand ");
	_outNode(str, node->lefthand);
	appendStringInfo(str, " :oper ");
	_outNode(str, node->oper);
	appendStringInfo(str, " :subselect ");
	_outNode(str, node->subselect);
}

/*
 *	Array is a subclass of Expr
 */
static void
_outArray(StringInfo str, Array *node)
{
	char		buf[500];
	int			i;

	appendStringInfo(str, " ARRAY ");
	sprintf(buf, " :arrayelemtype %u ", node->arrayelemtype);
	appendStringInfo(str, buf);
	sprintf(buf, " :arrayelemlength %d ", node->arrayelemlength);
	appendStringInfo(str, buf);
	sprintf(buf, " :arrayelembyval %c ", (node->arrayelembyval) ? 't' : 'f');
	appendStringInfo(str, buf);
	sprintf(buf, " :arrayndim %d ", node->arrayndim);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :arraylow ");
	for (i = 0; i < node->arrayndim; i++)
	{
		sprintf(buf, " %d ", node->arraylow.indx[i]);
		appendStringInfo(str, buf);
	}
	appendStringInfo(str, " :arrayhigh ");
	for (i = 0; i < node->arrayndim; i++)
	{
		sprintf(buf, " %d ", node->arrayhigh.indx[i]);
		appendStringInfo(str, buf);
	}
	sprintf(buf, " :arraylen %d ", node->arraylen);
	appendStringInfo(str, buf);
}

/*
 *	ArrayRef is a subclass of Expr
 */
static void
_outArrayRef(StringInfo str, ArrayRef *node)
{
	char		buf[500];

	appendStringInfo(str, " ARRAYREF ");
	sprintf(buf, " :refelemtype %u ", node->refelemtype);
	appendStringInfo(str, buf);
	sprintf(buf, " :refattrlength %d ", node->refattrlength);
	appendStringInfo(str, buf);
	sprintf(buf, " :refelemlength %d ", node->refelemlength);
	appendStringInfo(str, buf);
	sprintf(buf, " :refelembyval %c ", (node->refelembyval) ? 't' : 'f');
	appendStringInfo(str, buf);

	appendStringInfo(str, " :refupperindex ");
	_outNode(str, node->refupperindexpr);

	appendStringInfo(str, " :reflowerindex ");
	_outNode(str, node->reflowerindexpr);

	appendStringInfo(str, " :refexpr ");
	_outNode(str, node->refexpr);

	appendStringInfo(str, " :refassgnexpr ");
	_outNode(str, node->refassgnexpr);
}

/*
 *	Func is a subclass of Expr
 */
static void
_outFunc(StringInfo str, Func *node)
{
	char		buf[500];

	appendStringInfo(str, " FUNC ");
	sprintf(buf, " :funcid %u ", node->funcid);
	appendStringInfo(str, buf);
	sprintf(buf, " :functype %u ", node->functype);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :funcisindex ");
	appendStringInfo(str, (node->funcisindex ? "true" : "false"));
	sprintf(buf, " :funcsize %d ", node->funcsize);
	appendStringInfo(str, buf);
	sprintf(buf, " :func_fcache @ 0x%x ", (int) (node->func_fcache));
	appendStringInfo(str, buf);

	appendStringInfo(str, " :func_tlist ");
	_outNode(str, node->func_tlist);

	appendStringInfo(str, " :func_planlist ");
	_outNode(str, node->func_planlist);
}

/*
 *	Oper is a subclass of Expr
 */
static void
_outOper(StringInfo str, Oper *node)
{
	char		buf[500];

	appendStringInfo(str, " OPER ");
	sprintf(buf, " :opno %u ", node->opno);
	appendStringInfo(str, buf);
	sprintf(buf, " :opid %u ", node->opid);
	appendStringInfo(str, buf);
	sprintf(buf, " :opresulttype %u ", node->opresulttype);
	appendStringInfo(str, buf);

}

/*
 *	Param is a subclass of Expr
 */
static void
_outParam(StringInfo str, Param *node)
{
	char		buf[500];

	appendStringInfo(str, " PARAM ");
	sprintf(buf, " :paramkind %d ", node->paramkind);
	appendStringInfo(str, buf);
	sprintf(buf, " :paramid %d ", node->paramid);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :paramname ");
	appendStringInfo(str, node->paramname);
	sprintf(buf, " :paramtype %u ", node->paramtype);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :param_tlist ");
	_outNode(str, node->param_tlist);
}

/*
 *	Stuff from execnodes.h
 */

/*
 *	EState is a subclass of Node.
 */
static void
_outEState(StringInfo str, EState *node)
{
	char		buf[500];

	appendStringInfo(str, " ESTATE ");
	sprintf(buf, " :direction %d ", node->es_direction);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :range_table ");
	_outNode(str, node->es_range_table);

	sprintf(buf, " :result_relation_info @ 0x%x ",
			(int) (node->es_result_relation_info));
	appendStringInfo(str, buf);

}

/*
 *	Stuff from relation.h
 */
static void
_outRelOptInfo(StringInfo str, RelOptInfo * node)
{
	char		buf[500];

	appendStringInfo(str, " RELOPTINFO ");

	appendStringInfo(str, " :relids ");
	_outIntList(str, node->relids);

	appendStringInfo(str, " :indexed ");
	appendStringInfo(str, node->indexed ? "true" : "false");
	sprintf(buf, " :pages %u ", node->pages);
	appendStringInfo(str, buf);
	sprintf(buf, " :tuples %u ", node->tuples);
	appendStringInfo(str, buf);
	sprintf(buf, " :size %u ", node->size);
	appendStringInfo(str, buf);
	sprintf(buf, " :width %u ", node->width);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :targetlist ");
	_outNode(str, node->targetlist);

	appendStringInfo(str, " :pathlist ");
	_outNode(str, node->pathlist);

	/*
	 * Not sure if these are nodes or not.	They're declared as struct
	 * Path *.	Since i don't know, i'll just print the addresses for now.
	 * This can be changed later, if necessary.
	 */

	sprintf(buf, " :unorderedpath @ 0x%x ", (int) (node->unorderedpath));
	appendStringInfo(str, buf);
	sprintf(buf, " :cheapestpath @ 0x%x ", (int) (node->cheapestpath));
	appendStringInfo(str, buf);

	appendStringInfo(str, " :pruneable ");
	appendStringInfo(str, node->pruneable ? "true" : "false");

#if 0
	appendStringInfo(str, " :classlist ");
	_outNode(str, node->classlist);

	appendStringInfo(str, " :indexkeys ");
	_outNode(str, node->indexkeys);

	appendStringInfo(str, " :ordering ");
	_outNode(str, node->ordering);
#endif

	appendStringInfo(str, " :clauseinfo ");
	_outNode(str, node->clauseinfo);

	appendStringInfo(str, " :joininfo ");
	_outNode(str, node->joininfo);

	appendStringInfo(str, " :innerjoin ");
	_outNode(str, node->innerjoin);

}

/*
 *	TargetEntry is a subclass of Node.
 */
static void
_outTargetEntry(StringInfo str, TargetEntry *node)
{
	appendStringInfo(str, " TARGETENTRY ");
	appendStringInfo(str, " :resdom ");
	_outNode(str, node->resdom);

	appendStringInfo(str, " :expr ");
	_outNode(str, node->expr);
}

static void
_outRangeTblEntry(StringInfo str, RangeTblEntry *node)
{
	char		buf[500];

	appendStringInfo(str, " RTE ");

	appendStringInfo(str, " :relname ");
	appendStringInfo(str, node->relname);

	appendStringInfo(str, " :refname ");
	appendStringInfo(str, node->refname);

	sprintf(buf, " :relid %u ", node->relid);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :inh ");
	appendStringInfo(str, node->inh ? "true" : "false");

	appendStringInfo(str, " :inFromCl ");
	appendStringInfo(str, node->inFromCl ? "true" : "false");

	appendStringInfo(str, " :skipAcl ");
	appendStringInfo(str, node->skipAcl ? "true" : "false");

}

/*
 *	Path is a subclass of Node.
 */
static void
_outPath(StringInfo str, Path *node)
{
	char		buf[500];

	appendStringInfo(str, " PATH ");

	sprintf(buf, " :pathtype %d ", node->pathtype);
	appendStringInfo(str, buf);

	sprintf(buf, " :cost %f ", node->path_cost);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :keys ");
	_outNode(str, node->keys);

}

/*
 *	IndexPath is a subclass of Path.
 */
static void
_outIndexPath(StringInfo str, IndexPath *node)
{
	char		buf[500];

	appendStringInfo(str, " INDEXPATH ");

	sprintf(buf, " :pathtype %d ", node->path.pathtype);
	appendStringInfo(str, buf);

	/*
	 * sprintf(buf, " :parent "); appendStringInfo(str,buf); _outNode(str,
	 * node->parent);
	 */

	sprintf(buf, " :cost %f ", node->path.path_cost);
	appendStringInfo(str, buf);

#if 0
	appendStringInfo(str, " :p_ordering ");
	_outNode(str, node->path.p_ordering);
#endif
	appendStringInfo(str, " :keys ");
	_outNode(str, node->path.keys);

	appendStringInfo(str, " :indexid ");
	_outIntList(str, node->indexid);

	appendStringInfo(str, " :indexqual ");
	_outNode(str, node->indexqual);

}

/*
 *	JoinPath is a subclass of Path
 */
static void
_outJoinPath(StringInfo str, JoinPath *node)
{
	char		buf[500];

	appendStringInfo(str, " JOINPATH ");

	sprintf(buf, " :pathtype %d ", node->path.pathtype);
	appendStringInfo(str, buf);

	/*
	 * sprintf(buf, " :parent "); appendStringInfo(str,buf); _outNode(str,
	 * node->parent);
	 */

	sprintf(buf, " :cost %f ", node->path.path_cost);
	appendStringInfo(str, buf);

#if 0
	appendStringInfo(str, " :p_ordering ");
	_outNode(str, node->path.p_ordering);
#endif
	appendStringInfo(str, " :keys ");
	_outNode(str, node->path.keys);

	appendStringInfo(str, " :pathclauseinfo ");
	_outNode(str, node->pathclauseinfo);

	/*
	 * Not sure if these are nodes; they're declared as "struct path *".
	 * For now, i'll just print the addresses.
	 */

	sprintf(buf, " :outerjoinpath @ 0x%x ", (int) (node->outerjoinpath));
	appendStringInfo(str, buf);
	sprintf(buf, " :innerjoinpath @ 0x%x ", (int) (node->innerjoinpath));
	appendStringInfo(str, buf);

	sprintf(buf, " :outerjoincost %f ", node->path.outerjoincost);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :joinid ");
	_outIntList(str, node->path.joinid);

}

/*
 *	MergePath is a subclass of JoinPath.
 */
static void
_outMergePath(StringInfo str, MergePath *node)
{
	char		buf[500];

	appendStringInfo(str, " MERGEPATH ");

	sprintf(buf, " :pathtype %d ", node->jpath.path.pathtype);
	appendStringInfo(str, buf);

	sprintf(buf, " :cost %f ", node->jpath.path.path_cost);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :keys ");
	_outNode(str, node->jpath.path.keys);

	appendStringInfo(str, " :pathclauseinfo ");
	_outNode(str, node->jpath.pathclauseinfo);

	/*
	 * Not sure if these are nodes; they're declared as "struct path *".
	 * For now, i'll just print the addresses.
	 */

	sprintf(buf, " :outerjoinpath @ 0x%x ", (int) (node->jpath.outerjoinpath));
	appendStringInfo(str, buf);
	sprintf(buf, " :innerjoinpath @ 0x%x ", (int) (node->jpath.innerjoinpath));
	appendStringInfo(str, buf);

	sprintf(buf, " :outerjoincost %f ", node->jpath.path.outerjoincost);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :joinid ");
	_outIntList(str, node->jpath.path.joinid);

	appendStringInfo(str, " :path_mergeclauses ");
	_outNode(str, node->path_mergeclauses);

	appendStringInfo(str, " :outersortkeys ");
	_outNode(str, node->outersortkeys);

	appendStringInfo(str, " :innersortkeys ");
	_outNode(str, node->innersortkeys);

}

/*
 *	HashPath is a subclass of JoinPath.
 */
static void
_outHashPath(StringInfo str, HashPath *node)
{
	char		buf[500];

	appendStringInfo(str, " HASHPATH ");

	sprintf(buf, " :pathtype %d ", node->jpath.path.pathtype);
	appendStringInfo(str, buf);

	sprintf(buf, " :cost %f ", node->jpath.path.path_cost);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :keys ");
	_outNode(str, node->jpath.path.keys);

	appendStringInfo(str, " :pathclauseinfo ");
	_outNode(str, node->jpath.pathclauseinfo);

	/*
	 * Not sure if these are nodes; they're declared as "struct path *".
	 * For now, i'll just print the addresses.
	 */

	sprintf(buf, " :outerjoinpath @ 0x%x ", (int) (node->jpath.outerjoinpath));
	appendStringInfo(str, buf);
	sprintf(buf, " :innerjoinpath @ 0x%x ", (int) (node->jpath.innerjoinpath));
	appendStringInfo(str, buf);

	sprintf(buf, " :outerjoincost %f ", node->jpath.path.outerjoincost);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :joinid ");
	_outIntList(str, node->jpath.path.joinid);

	appendStringInfo(str, " :path_hashclauses ");
	_outNode(str, node->path_hashclauses);

	appendStringInfo(str, " :outerhashkeys ");
	_outNode(str, node->outerhashkeys);

	appendStringInfo(str, " :innerhashkeys ");
	_outNode(str, node->innerhashkeys);

}

/*
 *	OrderKey is a subclass of Node.
 */
static void
_outOrderKey(StringInfo str, OrderKey *node)
{
	char		buf[500];

	appendStringInfo(str, " ORDERKEY ");
	sprintf(buf, " :attribute_number %d ", node->attribute_number);
	appendStringInfo(str, buf);
	sprintf(buf, " :array_index %d ", node->array_index);
	appendStringInfo(str, buf);

}

/*
 *	JoinKey is a subclass of Node.
 */
static void
_outJoinKey(StringInfo str, JoinKey *node)
{
	appendStringInfo(str, " JOINKEY ");

	appendStringInfo(str, " :outer ");
	_outNode(str, node->outer);

	appendStringInfo(str, " :inner ");
	_outNode(str, node->inner);

}

/*
 *	MergeOrder is a subclass of Node.
 */
static void
_outMergeOrder(StringInfo str, MergeOrder *node)
{
	char		buf[500];

	appendStringInfo(str, " MERGEORDER ");

	sprintf(buf, " :join_operator %d ", node->join_operator);
	appendStringInfo(str, buf);
	sprintf(buf, " :left_operator %d ", node->left_operator);
	appendStringInfo(str, buf);
	sprintf(buf, " :right_operator %d ", node->right_operator);
	appendStringInfo(str, buf);
	sprintf(buf, " :left_type %d ", node->left_type);
	appendStringInfo(str, buf);
	sprintf(buf, " :right_type %d ", node->right_type);
	appendStringInfo(str, buf);

}

/*
 *	ClauseInfo is a subclass of Node.
 */
static void
_outClauseInfo(StringInfo str, ClauseInfo * node)
{
	char		buf[500];

	appendStringInfo(str, " CINFO ");

	appendStringInfo(str, " :clause ");
	_outNode(str, node->clause);

	sprintf(buf, " :selectivity %f ", node->selectivity);
	appendStringInfo(str, buf);
	appendStringInfo(str, " :notclause ");
	appendStringInfo(str, node->notclause ? "true" : "false");

	appendStringInfo(str, " :indexids ");
	_outNode(str, node->indexids);

	appendStringInfo(str, " :mergejoinorder ");
	_outNode(str, node->mergejoinorder);

	sprintf(buf, " :hashjoinoperator %u ", node->hashjoinoperator);
	appendStringInfo(str, buf);

}

/*
 *	JoinMethod is a subclass of Node.
 */
static void
_outJoinMethod(StringInfo str, JoinMethod *node)
{
	appendStringInfo(str, " JOINMETHOD ");

	appendStringInfo(str, " :jmkeys ");
	_outNode(str, node->jmkeys);

	appendStringInfo(str, " :clauses ");
	_outNode(str, node->clauses);


}

/*
 * HInfo is a subclass of JoinMethod.
 */
static void
_outHInfo(StringInfo str, HInfo *node)
{
	char		buf[500];

	appendStringInfo(str, " HASHINFO ");

	appendStringInfo(str, " :hashop ");
	sprintf(buf, " %u ", node->hashop);
	appendStringInfo(str, buf);

	appendStringInfo(str, " :jmkeys ");
	_outNode(str, node->jmethod.jmkeys);

	appendStringInfo(str, " :clauses ");
	_outNode(str, node->jmethod.clauses);

}

/*
 *	JoinInfo is a subclass of Node.
 */
static void
_outJoinInfo(StringInfo str, JoinInfo * node)
{
	appendStringInfo(str, " JINFO ");

	appendStringInfo(str, " :otherrels ");
	_outIntList(str, node->otherrels);

	appendStringInfo(str, " :jinfoclauseinfo ");
	_outNode(str, node->jinfoclauseinfo);

	appendStringInfo(str, " :mergejoinable ");
	appendStringInfo(str, node->mergejoinable ? "true" : "false");
	appendStringInfo(str, " :hashjoinable ");
	appendStringInfo(str, node->hashjoinable ? "true" : "false");

}

/*
 * Print the value of a Datum given its type.
 */
static void
_outDatum(StringInfo str, Datum value, Oid type)
{
	char		buf[500];
	Size		length,
				typeLength;
	bool		byValue;
	int			i;
	char	   *s;

	/*
	 * find some information about the type and the "real" length of the
	 * datum.
	 */
	byValue = get_typbyval(type);
	typeLength = get_typlen(type);
	length = datumGetSize(value, type, byValue, typeLength);

	if (byValue)
	{
		s = (char *) (&value);
		sprintf(buf, " %d [ ", length);
		appendStringInfo(str, buf);
		for (i = 0; i < sizeof(Datum); i++)
		{
			sprintf(buf, " %d ", (int) (s[i]));
			appendStringInfo(str, buf);
		}
		sprintf(buf, "] ");
		appendStringInfo(str, buf);
	}
	else
	{							/* !byValue */
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
		{
			sprintf(buf, " 0 [ ] ");
			appendStringInfo(str, buf);
		}
		else
		{

			/*
			 * length is unsigned - very bad to do < comparison to -1
			 * without casting it to int first!! -mer 8 Jan 1991
			 */
			if (((int) length) <= -1)
				length = VARSIZE(s);
			sprintf(buf, " %d [ ", length);
			appendStringInfo(str, buf);
			for (i = 0; i < length; i++)
			{
				sprintf(buf, " %d ", (int) (s[i]));
				appendStringInfo(str, buf);
			}
			sprintf(buf, "] ");
			appendStringInfo(str, buf);
		}
	}
}

static void
_outIter(StringInfo str, Iter *node)
{
	appendStringInfo(str, " ITER ");

	appendStringInfo(str, " :iterexpr ");
	_outNode(str, node->iterexpr);
}

static void
_outStream(StringInfo str, Stream *node)
{
	char		buf[500];

	appendStringInfo(str, " STREAM ");

	sprintf(buf, " :pathptr @ 0x%x ", (int) (node->pathptr));
	appendStringInfo(str, buf);

	sprintf(buf, " :cinfo @ 0x%x ", (int) (node->cinfo));
	appendStringInfo(str, buf);

	sprintf(buf, " :clausetype %d ", (int) (node->clausetype));
	appendStringInfo(str, buf);

	sprintf(buf, " :upstream @ 0x%x ", (int) (node->upstream));
	appendStringInfo(str, buf);

	sprintf(buf, " :downstream @ 0x%x ", (int) (node->downstream));
	appendStringInfo(str, buf);

	sprintf(buf, " :groupup %d ", node->groupup);
	appendStringInfo(str, buf);

	sprintf(buf, " :groupcost %f ", node->groupcost);
	appendStringInfo(str, buf);

	sprintf(buf, " :groupsel %f ", node->groupsel);
	appendStringInfo(str, buf);
}

static void
_outAExpr(StringInfo str, A_Expr *node)
{
	appendStringInfo(str, "EXPR ");
#ifdef PARSEDEBUG
	switch (node->oper)
	{
		case AND:
			appendStringInfo(str, "AND");
			break;
		case OR:
			appendStringInfo(str, "OR");
			break;
		case NOT:
			appendStringInfo(str, "NOT");
			break;
		case ISNULL:
			appendStringInfo(str, "ISNULL");
			break;
		case NOTNULL:
			appendStringInfo(str, "NOTNULL");
			break;
		default:
#endif
			appendStringInfo(str, node->opname);
#ifdef PARSEDEBUG
			break;
	}
#endif
	_outNode(str, node->lexpr);
	_outNode(str, node->rexpr);
	return;
}

static void
_outValue(StringInfo str, Value *value)
{
	char		buf[500];

	switch (value->type)
	{
		case T_String:
			sprintf(buf, " \"%s\" ", value->val.str);
			appendStringInfo(str, buf);
			break;
		case T_Integer:
			sprintf(buf, " %ld ", value->val.ival);
			appendStringInfo(str, buf);
			break;
		case T_Float:
			sprintf(buf, " %f ", value->val.dval);
			appendStringInfo(str, buf);
			break;
		default:
			break;
	}
	return;
}

static void
_outIdent(StringInfo str, Ident *node)
{
	char		buf[500];

	sprintf(buf, " IDENT \"%s\" ", node->name);
	appendStringInfo(str, buf);
	return;
}

static void
_outAConst(StringInfo str, A_Const *node)
{
	char		buf[500];

	sprintf(buf, "CONST ");
	appendStringInfo(str, buf);
	_outValue(str, &(node->val));
	return;
}

static void
_outConstraint(StringInfo str, Constraint *node)
{
	char		buf[500];

	sprintf(buf," %s :type",
		((node->name != NULL)? node->name: "<>"));
	appendStringInfo(str, buf);

	switch (node->contype)
	{
		case CONSTR_PRIMARY:
			sprintf(buf," PRIMARY KEY ");
			appendStringInfo(str, buf);
			_outNode(str, node->keys);
			break;

		case CONSTR_CHECK:
			sprintf(buf," CHECK ");
			appendStringInfo(str, buf);
			appendStringInfo(str, node->def);
			break;

		case CONSTR_DEFAULT:
			sprintf(buf," DEFAULT ");
			appendStringInfo(str, buf);
			appendStringInfo(str, node->def);
			break;

		case CONSTR_NOTNULL:
			sprintf(buf," NOT NULL ");
			appendStringInfo(str, buf);
			break;

		case CONSTR_UNIQUE:
			sprintf(buf," UNIQUE ");
			appendStringInfo(str, buf);
			_outNode(str, node->keys);
			break;

		default:
			sprintf(buf,"<unrecognized constraint>");
			appendStringInfo(str, buf);
			break;
	}
	return;
}

static void
_outCaseExpr(StringInfo str, CaseExpr *node)
{
	char		buf[500];

	sprintf(buf, "CASE ");
	appendStringInfo(str, buf);
	_outNode(str, node->args);
	sprintf(buf, " :default ");
	appendStringInfo(str, buf);
	_outNode(str, node->defresult);
	return;
}

static void
_outCaseWhen(StringInfo str, CaseWhen *node)
{
	char		buf[500];

	sprintf(buf, " WHEN ");
	appendStringInfo(str, buf);
	_outNode(str, node->expr);
	sprintf(buf, " :then ");
	appendStringInfo(str, buf);
	_outNode(str, node->result);
	return;
}

/*
 * _outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
static void
_outNode(StringInfo str, void *obj)
{
	if (obj == NULL)
	{
		appendStringInfo(str, "<>");
		return;
	}

	if (nodeTag(obj) == T_List)
	{
		List	   *l;

		appendStringInfo(str, "(");
		foreach(l, (List *) obj)
		{
			_outNode(str, lfirst(l));
			if (lnext(l))
				appendStringInfo(str, " ");
		}
		appendStringInfo(str, ")");
	}
	else
	{
		appendStringInfo(str, "{");
		switch (nodeTag(obj))
		{
			case T_CreateStmt:
				_outCreateStmt(str, obj);
				break;
			case T_IndexStmt:
				_outIndexStmt(str, obj);
				break;

			case T_ColumnDef:
				_outColumnDef(str, obj);
				break;
			case T_TypeName:
				_outTypeName(str, obj);
				break;
			case T_IndexElem:
				_outIndexElem(str, obj);
				break;

#ifdef PARSEDEBUG
			case T_VariableSetStmt:
				break;
			case T_SelectStmt:
				_outSelectStmt(str, obj);
				break;
			case T_FuncCall:
				_outFuncCall(str, obj);
				break;
#endif

			case T_Query:
				_outQuery(str, obj);
				break;
			case T_SortClause:
				_outSortClause(str, obj);
				break;
			case T_GroupClause:
				_outGroupClause(str, obj);
				break;
			case T_Plan:
				_outPlan(str, obj);
				break;
			case T_Result:
				_outResult(str, obj);
				break;
			case T_Append:
				_outAppend(str, obj);
				break;
			case T_Join:
				_outJoin(str, obj);
				break;
			case T_NestLoop:
				_outNestLoop(str, obj);
				break;
			case T_MergeJoin:
				_outMergeJoin(str, obj);
				break;
			case T_HashJoin:
				_outHashJoin(str, obj);
				break;
			case T_Scan:
				_outScan(str, obj);
				break;
			case T_SeqScan:
				_outSeqScan(str, obj);
				break;
			case T_IndexScan:
				_outIndexScan(str, obj);
				break;
			case T_Temp:
				_outTemp(str, obj);
				break;
			case T_Sort:
				_outSort(str, obj);
				break;
			case T_Agg:
				_outAgg(str, obj);
				break;
			case T_Group:
				_outGroup(str, obj);
				break;
			case T_Unique:
				_outUnique(str, obj);
				break;
			case T_Hash:
				_outHash(str, obj);
				break;
			case T_SubPlan:
				_outSubPlan(str, obj);
				break;
			case T_Tee:
				_outTee(str, obj);
				break;
			case T_Resdom:
				_outResdom(str, obj);
				break;
			case T_Fjoin:
				_outFjoin(str, obj);
				break;
			case T_Expr:
				_outExpr(str, obj);
				break;
			case T_Var:
				_outVar(str, obj);
				break;
			case T_Const:
				_outConst(str, obj);
				break;
			case T_Aggreg:
				_outAggreg(str, obj);
				break;
			case T_SubLink:
				_outSubLink(str, obj);
				break;
			case T_Array:
				_outArray(str, obj);
				break;
			case T_ArrayRef:
				_outArrayRef(str, obj);
				break;
			case T_Func:
				_outFunc(str, obj);
				break;
			case T_Oper:
				_outOper(str, obj);
				break;
			case T_Param:
				_outParam(str, obj);
				break;
			case T_EState:
				_outEState(str, obj);
				break;
			case T_RelOptInfo:
				_outRelOptInfo(str, obj);
				break;
			case T_TargetEntry:
				_outTargetEntry(str, obj);
				break;
			case T_RangeTblEntry:
				_outRangeTblEntry(str, obj);
				break;
			case T_Path:
				_outPath(str, obj);
				break;
			case T_IndexPath:
				_outIndexPath(str, obj);
				break;
			case T_JoinPath:
				_outJoinPath(str, obj);
				break;
			case T_MergePath:
				_outMergePath(str, obj);
				break;
			case T_HashPath:
				_outHashPath(str, obj);
				break;
			case T_OrderKey:
				_outOrderKey(str, obj);
				break;
			case T_JoinKey:
				_outJoinKey(str, obj);
				break;
			case T_MergeOrder:
				_outMergeOrder(str, obj);
				break;
			case T_ClauseInfo:
				_outClauseInfo(str, obj);
				break;
			case T_JoinMethod:
				_outJoinMethod(str, obj);
				break;
			case T_HInfo:
				_outHInfo(str, obj);
				break;
			case T_JoinInfo:
				_outJoinInfo(str, obj);
				break;
			case T_Iter:
				_outIter(str, obj);
				break;
			case T_Stream:
				_outStream(str, obj);
				break;
			case T_Integer:
			case T_String:
			case T_Float:
				_outValue(str, obj);
				break;
			case T_A_Expr:
				_outAExpr(str, obj);
				break;
			case T_Ident:
				_outIdent(str, obj);
				break;
			case T_A_Const:
				_outAConst(str, obj);
				break;
			case T_Constraint:
				_outConstraint(str, obj);
				break;
			case T_CaseExpr:
				_outCaseExpr(str, obj);
				break;
			case T_CaseWhen:
				_outCaseWhen(str, obj);
				break;
			default:
				elog(NOTICE, "_outNode: don't know how to print type %d ",
					 nodeTag(obj));
				break;
		}
		appendStringInfo(str, "}");
	}
	return;
}

/*
 * nodeToString -
 *	   returns the ascii representation of the Node
 */
char *
nodeToString(void *obj)
{
	StringInfo	str;
	char	   *s;

	if (obj == NULL)
		return "";
	Assert(obj != NULL);
	str = makeStringInfo();
	_outNode(str, obj);
	s = str->data;
	pfree(str);

	return s;
}
