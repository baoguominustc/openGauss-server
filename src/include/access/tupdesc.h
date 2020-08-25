/* -------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES tuple descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "postgres.h"
#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"

typedef struct attrDefault {
    AttrNumber adnum;
    char* adbin; /* nodeToString representation of expr */
} AttrDefault;

typedef struct constrCheck {
    char* ccname;
    char* ccbin; /* nodeToString representation of expr */
    bool ccvalid;
    bool ccnoinherit; /* this is a non-inheritable constraint */
} ConstrCheck;

/* This structure contains constraints of a tuple */
typedef struct tupleConstr {
    AttrDefault* defval;     /* array */
    ConstrCheck* check;      /* array */
    AttrNumber* clusterKeys; /* cluster keys */
    uint16 clusterKeyNum;
    uint16 num_defval;
    uint16 num_check;
    bool has_not_null;
} TupleConstr;

/* This structure contains initdefval of a tuple */
typedef struct tupInitDefVal {
    Datum* datum;
    bool isNull;
    uint16 dataLen;
} TupInitDefVal;

/*
 * @hdfs
 * This strrcture constains the following informations about on
 * informational constraint:
 * 1. constrname records informational constaint name.
 * 2. contype records the informational constraint type.
 *    'p' represnets pirmary key constraint. 'u' represents unique
 *    constraint.
 * 3. nonforced records the enforced or forced attributtes of informationanl constaint.
 * 4. enableOpt records the enalbe or disbale query optimization attribute of informational constaint.
 */
typedef struct InformationalConstraint {
    NodeTag type;
    char* constrname;
    char contype;
    bool nonforced;
    bool enableOpt;
} InformationalConstraint;

/*
 * This struct is passed around within the backend to describe the structure
 * of tuples.  For tuples coming from on-disk relations, the information is
 * collected from the pg_attribute, pg_attrdef, and pg_constraint catalogs.
 * Transient row types (such as the result of a join query) have anonymous
 * TupleDesc structs that generally omit any constraint info; therefore the
 * structure is designed to let the constraints be omitted efficiently.
 *
 * Note that only user attributes, not system attributes, are mentioned in
 * TupleDesc; with the exception that tdhasoid indicates if OID is present.
 *
 * If the tupdesc is known to correspond to a named rowtype (such as a table's
 * rowtype) then tdtypeid identifies that type and tdtypmod is -1.	Otherwise
 * tdtypeid is RECORDOID, and tdtypmod can be either -1 for a fully anonymous
 * row type, or a value >= 0 to allow the rowtype to be looked up in the
 * typcache.c type cache.
 *
 * Tuple descriptors that live in caches (relcache or typcache, at present)
 * are reference-counted: they can be deleted when their reference count goes
 * to zero.  Tuple descriptors created by the executor need no reference
 * counting, however: they are simply created in the appropriate memory
 * context and go away when the context is freed.  We set the tdrefcount
 * field of such a descriptor to -1, while reference-counted descriptors
 * always have tdrefcount >= 0.
 */
typedef struct tupleDesc {
    int natts; /* number of attributes in the tuple */
    bool tdisredistable; /* temp table created for data redistribution by the redis tool */
    Form_pg_attribute* attrs;
    /* attrs[N] is a pointer to the description of Attribute Number N+1 */
    TupleConstr* constr;        /* constraints, or NULL if none */
    TupInitDefVal* initdefvals; /* init default value due to ADD COLUMN */
    Oid tdtypeid;               /* composite type ID for tuple type */
    int32 tdtypmod;             /* typmod for tuple type */
    bool tdhasoid;              /* tuple has oid attribute in its header */
    int tdrefcount;             /* reference count, or -1 if not counting */
} * TupleDesc;

/* Accessor for the i'th attribute of tupdesc. */
#define TupleDescAttr(tupdesc, i) ((tupdesc)->attrs[(i)])

extern TupleDesc CreateTemplateTupleDesc(int natts, bool hasoid);

extern TupleDesc CreateTupleDesc(int natts, bool hasoid, Form_pg_attribute* attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern void IncrTupleDescRefCount(TupleDesc tupdesc);
extern void DecrTupleDescRefCount(TupleDesc tupdesc);

#define PinTupleDesc(tupdesc)               \
    do {                                    \
        if ((tupdesc)->tdrefcount >= 0)     \
            IncrTupleDescRefCount(tupdesc); \
    } while (0)

#define ReleaseTupleDesc(tupdesc)           \
    do {                                    \
        if ((tupdesc)->tdrefcount >= 0)     \
            DecrTupleDescRefCount(tupdesc); \
    } while (0)

extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
extern bool equalDeltaTupleDescs(TupleDesc main_tupdesc, TupleDesc delta_tupdesc);

extern void TupleDescInitEntry(
    TupleDesc desc, AttrNumber attribute_number, const char* attribute_name, Oid oidtypeid, int32 typmod, int attdim);

extern void TupleDescInitEntryCollation(TupleDesc desc, AttrNumber attribute_number, Oid collationid);

extern void VerifyAttrCompressMode(int8 mode, int attlen, const char* attname);

extern TupleDesc BuildDescForRelation(List* schema, Node* oriented_from, char relkind = '\0');

extern TupleDesc BuildDescFromLists(List* names, List* types, List* typmods, List* collations);

extern bool tupledesc_have_pck(TupleConstr* constr);

extern void copyDroppedAttribute(Form_pg_attribute target, Form_pg_attribute source);

#endif /* TUPDESC_H */
