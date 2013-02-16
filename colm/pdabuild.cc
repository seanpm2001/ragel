/*
 *  Copyright 2006-2012 Adrian Thurston <thurston@complang.org>
 */

/*  This file is part of Colm.
 *
 *  Colm is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 * 
 *  Colm is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with Colm; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

#include <iostream>
#include <iomanip>
#include <errno.h>
#include <stdlib.h>

/* Parsing. */
#include "global.h"
#include "parsedata.h"
#include "pdacodegen.h"
#include "pdarun.h"
#include "redfsm.h"
#include "fsmcodegen.h"
#include "redbuild.h"

/* Dumping the fsm. */
#include "mergesort.h"

using std::endl;
using std::cerr;
using std::cout;

char startDefName[] = "start";

/* Count the transitions in the fsm by walking the state list. */
int countTransitions( PdaGraph *fsm )
{
	int numTrans = 0;
	PdaState *state = fsm->stateList.head;
	while ( state != 0 ) {
		numTrans += state->transMap.length();
		state = state->next;
	}
	return numTrans;
}

LangEl::LangEl( Namespace *nspace, const String &name, Type type )
:
	nspace(nspace),
	name(name),
	lit(name),
	type(type),
	id(-1),
	isUserTerm(false),
	isContext(false),
	displayString(0),
	numAppearances(0),
	commit(false),
	ignore(false),
	reduceFirst(false),
	isLiteral(false),
	isRepeat(false),
	isList(false),
	isOpt(false),
	parseStop(false),
	isEOF(false),
	repeatOf(0),
	tokenDef(0),
	rootDef(0),
	termDup(0),
	eofLel(0),
	pdaGraph(0),
	pdaTables(0),
	transBlock(0),
	objectDef(0),
	thisSize(0),
	ofiOffset(0),
	generic(0),
	parserId(-1),
	predType(PredNone),
	predValue(0),
	contextDef(0),
	contextIn(0), 
	noPreIgnore(false),
	noPostIgnore(false),
	isCI(false),
	ciRegion(0)
{
}
 
PdaGraph *ProdElList::walk( Compiler *pd, Production *prod )
{
	PdaGraph *prodFsm = new PdaGraph();
	PdaState *last = prodFsm->addState();
	prodFsm->setStartState( last );

	if ( prod->collectIgnoreRegion != 0 ) {
//		cerr << "production " << prod->data << " has collect ignore region " << 
//				prod->collectIgnoreRegion->name << endl;

		/* Use the IGNORE TOKEN lang el for the region. */
		long value = prod->collectIgnoreRegion->ciLel->id;

		PdaState *newState = prodFsm->addState();
		PdaTrans *newTrans = prodFsm->appendNewTrans( last, newState, value, value );

		newTrans->isShift = true;
		newTrans->shiftPrior = 0; // WAT
		last = newState;
	}

	int prodLength = 0;
	for ( Iter prodEl = first(); prodEl.lte(); prodEl++, prodLength++ ) {
		//PdaGraph *itemFsm = prodEl->walk( pd );
		long value = prodEl->langEl->id;

		PdaState *newState = prodFsm->addState();
		PdaTrans *newTrans = prodFsm->appendNewTrans( last, newState, value, value );

		newTrans->isShift = true;
		newTrans->shiftPrior = prodEl->priorVal;
		//cerr << "PRIOR VAL: " << newTrans->shiftPrior << endl;

		if ( prodEl->commit ) {
			//cout << "COMMIT: inserting commit of length: " << pd->prodLength << endl;
			/* Insert the commit into transitions out of last */
			for ( TransMap::Iter trans = last->transMap; trans.lte(); trans++ )
				trans->value->commits.insert( prodLength );
		}

		last = newState;
	}

	/* Make the last state the final state. */
	prodFsm->setFinState( last );
	return prodFsm;
}


ProdElList *Compiler::makeProdElList( LangEl *langEl )
{
	ProdElList *prodElList = new ProdElList();
	UniqueType *uniqueType = findUniqueType( TYPE_TREE, langEl );
	TypeRef *typeRef = TypeRef::cons( internal, uniqueType );
	prodElList->append( new ProdEl( internal, typeRef ) );
	prodElList->tail->langEl = langEl;
	return prodElList;
}

void Compiler::makeDefinitionNames()
{
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		int prodNum = 1;
		for ( LelDefList::Iter def = lel->defList; def.lte(); def++ ) {
			def->data.setAs( lel->name.length() + 32, "%s-%i", 
					lel->name.data, prodNum++ );
		}
	}
}

/* Make sure there there are no language elements whose type is unkonwn. This
 * can happen when an id is used on the rhs of a definition but is not defined
 * as anything. */
void Compiler::noUndefindLangEls()
{
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		if ( lel->type == LangEl::Unknown )
			error() << "'" << lel->name << "' was not defined as anything" << endp;
	}
}

void Compiler::makeLangElIds()
{
	/* The first id 0 is reserved for the stack sentinal. A negative id means
	 * error to the parsing function, inducing backtracking. */
	nextSymbolId = 1;

	/* First pass assigns to the user terminals. */
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		/* Must be a term, and not any of the special reserved terminals.
		 * Remember if the non terminal is a user non terminal. */
		if ( lel->type == LangEl::Term && 
				!lel->isEOF && 
				lel != errorLangEl &&
				lel != noTokenLangEl )
		{
			lel->isUserTerm = true;
			lel->id = nextSymbolId++;
		}
	}

	//eofLangEl->id = nextSymbolId++;
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		/* Must be a term, and not any of the special reserved terminals.
		 * Remember if the non terminal is a user non terminal. */
		if ( lel->isEOF )
			lel->id = nextSymbolId++;
	}

	/* Next assign to the eof notoken, which we always create. */
	noTokenLangEl->id = nextSymbolId++;

	/* Possibly assign to the error language element. */
	if ( errorLangEl != 0 )
		errorLangEl->id = nextSymbolId++;

	/* Save this for the code generation. */
	firstNonTermId = nextSymbolId;

	/* A third and final pass assigns to everything else. */
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		/* Anything else not yet assigned gets assigned now. */
		if ( lel->id < 0 )
			lel->id = nextSymbolId++;
	}

	assert( ptrLangEl->id == LEL_ID_PTR );
	assert( boolLangEl->id == LEL_ID_BOOL );
	assert( intLangEl->id == LEL_ID_INT );
	assert( strLangEl->id == LEL_ID_STR );
	assert( streamLangEl->id == LEL_ID_STREAM );
	assert( ignoreLangEl->id == LEL_ID_IGNORE );
}

void Compiler::refNameSpace( LangEl *lel, Namespace *nspace )
{
	if ( nspace == defaultNamespace || nspace == rootNamespace ) {
		lel->refName = "::" + lel->refName;
		return;
	}
	
	lel->refName = nspace->name + "::" + lel->refName;
	lel->declName = nspace->name + "::" + lel->declName;
	lel->xmlTag = nspace->name + "::" + lel->xmlTag;
	refNameSpace( lel, nspace->parentNamespace );
}

void Compiler::makeLangElNames()
{
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		if ( lel->id == LEL_ID_INT ) {
			lel->fullName = "_int";
			lel->fullLit = "_int";
			lel->refName = "_int";
			lel->declName = "_int";
			lel->xmlTag = "int";
		}
		else if ( lel->id == LEL_ID_BOOL ) {
			lel->fullName = "_bool";
			lel->fullLit = "_bool";
			lel->refName = "_bool";
			lel->declName = "_bool";
			lel->xmlTag = "bool";
		}
		else {
			lel->fullName = lel->name;
			lel->fullLit = lel->lit;
			lel->refName = lel->lit;
			lel->declName = lel->lit;
			lel->xmlTag = lel->name;
		}

		/* If there is also a namespace next to the type, we add a prefix to
		 * the type. It's not convenient to name C++ classes the same as a
		 * namespace in the same scope. We don't want to restrict colm, so we
		 * add a workaround for the least-common case. The type gets t_ prefix.
		 * */
		Namespace *nspace = lel->nspace->findNamespace( lel->name );
		if ( nspace != 0 ) {
			lel->refName = "t_" + lel->refName;
			lel->fullName = "t_" + lel->fullName;
			lel->declName = "t_" + lel->declName;
			lel->xmlTag = "t_" + lel->xmlTag;
		}

		refNameSpace( lel, lel->nspace );
	}
}

/* Set up dot sets, shift info, and prod sets. */
void Compiler::makeProdFsms()
{
	/* There are two items in the index for each production (high and low). */
	int indexLen = prodList.length() * 2;
	dotItemIndex.setAsNew( indexLen );
	int dsiLow = 0, indexPos = 0;

	/* Build FSMs for all production language elements. */
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ )
		prod->fsm = prod->prodElList->walk( this, prod );

	makeNonTermFirstSets();
	makeFirstSets();

	/* Build FSMs for all production language elements. */
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		if ( addUniqueEmptyProductions ) {
			/* This must be re-implemented. */
			assert( false );
			//if ( !prod->isLeftRec && prod->uniqueEmptyLeader != 0 ) {
			//	PdaGraph *emptyLeader = prod->uniqueEmptyLeader->walk( this );
			//	emptyLeader->concatOp( prod->fsm );
			//	prod->fsm = emptyLeader;
			//}
		}

		/* Compute the machine's length. */
		prod->fsmLength = prod->fsm->fsmLength( );

		/* Productions have a unique production id for each final state.
		 * This lets us use a production length specific to each final state.
		 * Start states are always isolated therefore if the start state is
		 * final then reductions from it will always have a fixed production
		 * length. This is a simple method for determining the length
		 * of zero-length derivations when reducing. */

		/* Number of dot items needed for the production is elements + 1
		 * because the dot can be before the first and after the last element. */
		int numForProd = prod->fsm->stateList.length() + 1;

		/* Set up the low and high values in the index for this production. */
		dotItemIndex.data[indexPos].key = dsiLow;
		dotItemIndex.data[indexPos].value = prod;
		dotItemIndex.data[indexPos+1].key = dsiLow + numForProd - 1;
		dotItemIndex.data[indexPos+1].value = prod;

		int dsi = dsiLow;
		for ( PdaStateList::Iter state = prod->fsm->stateList; state.lte(); state++, dsi++ ) {
			/* All transitions are shifts. */
			for ( TransMap::Iter out = state->transMap; out.lte(); out++ )
				assert( out->value->isShift );

			state->dotSet.insert( dsi );
		}

		/* Move over the production. */
		dsiLow += numForProd;
		indexPos += 2;

		if ( prod->prodCommit ) {
			for ( PdaStateSet::Iter fin = prod->fsm->finStateSet; fin.lte(); fin++ ) {
				int length = prod->fsmLength;
				//cerr << "PENDING COMMIT IN FINAL STATE of " << prod->prodId <<
				//		" with len: " << length << endl;
				(*fin)->pendingCommits.insert( ProdIdPair( prod->prodId, length ) );
			}
		}
	}

	/* Make the final state specific prod id to prod id mapping. */
	prodIdIndex = new Production*[prodList.length()];
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ )
		prodIdIndex[prod->prodId] = prod;
}

/* Want the first set of over src. If the first set contains epsilon, go over
 * it and over tab. If overSrc is the end of the production, find the follow
 * from the table, taking only the characters on which the parent is reduced.
 * */
void Compiler::findFollow( AlphSet &result, PdaState *overTab, 
		PdaState *overSrc, Production *parentDef )
{
	if ( overSrc->isFinState() ) {
		assert( overSrc->transMap.length() == 0 );

		/* At the end of the production. Turn to the table. */
		long redCode = makeReduceCode( parentDef->prodId, false );
		for ( TransMap::Iter tabTrans = overTab->transMap; tabTrans.lte(); tabTrans++ ) {
			for ( ActDataList::Iter adl = tabTrans->value->actions; adl.lte(); adl++ ) {
				if ( *adl == redCode )
					result.insert( tabTrans->key );
			}
		}
	}
	else {
		/* Get the first set of the item. If the first set contains epsilon
		 * then move over overSrc and overTab and recurse. */
		assert( overSrc->transMap.length() == 1 );
		TransMap::Iter pastTrans = overSrc->transMap;

		LangEl *langEl = langElIndex[pastTrans->key];
		if ( langEl != 0 && langEl->type == LangEl::NonTerm ) {
			bool hasEpsilon = false;
			for ( LelDefList::Iter def = langEl->defList; def.lte(); def++ ) {
				result.insert( def->firstSet );

				if ( def->firstSet.find( -1 ) )
					hasEpsilon = true;
			}

			/* Find the equivalent state in the parser. */
			if ( hasEpsilon ) {
				PdaTrans *tabTrans = overTab->findTrans( pastTrans->key );
				findFollow( result, tabTrans->toState, 
						pastTrans->value->toState, parentDef );
			}

			/* Now possibly the dup. */
			if ( langEl->termDup != 0 )
				result.insert( langEl->termDup->id );
		}
		else {
			result.insert( pastTrans->key );
		}
	}
}

PdaState *Compiler::followProd( PdaState *tabState, PdaState *prodState )
{
	while ( prodState->transMap.length() == 1 ) {
		TransMap::Iter prodTrans = prodState->transMap;
		PdaTrans *tabTrans = tabState->findTrans( prodTrans->key );
		prodState = prodTrans->value->toState;
		tabState = tabTrans->toState;
	}
	return tabState;
}

void Compiler::trySetTime( PdaTrans *trans, long code, long &time )
{
	/* Find the item. */
	for ( ActDataList::Iter adl = trans->actions; adl.lte(); adl++ ) {
		if ( *adl == code ) {
			/* If the time of the shift is not already set, set it. */
			if ( trans->actOrds[adl.pos()] == 0 ) {
				//cerr << "setting time: state = " << tabState->stateNum 
				//		<< ", trans = " << tabTrans->lowKey
				//		<< ", time = " << time << endl;
				trans->actOrds[adl.pos()] = time++;
			}
			break;
		}
	}
}

/* Go down a defintiion and then handle the follow actions. */
void Compiler::pdaOrderFollow( LangEl *rootEl, PdaState *tabState, 
		PdaTrans *tabTrans, PdaTrans *srcTrans, Production *parentDef, 
		Production *definition, long &time )
{
	/* We need the follow from tabState/srcState over the defintion we are
	 * currently processing. */
	PdaState *overTab = tabTrans->toState;
	PdaState *overSrc = srcTrans->toState;

	AlphSet alphSet;
	if ( parentDef == rootEl->rootDef )
		alphSet.insert( rootEl->eofLel->id );
	else
		findFollow( alphSet, overTab, overSrc, parentDef );		

	/* Now follow the production to find out where it expands to. */
	PdaState *expandToState = followProd( tabState, definition->fsm->startState );

	/* Find the reduce item. */
	long redCode = makeReduceCode( definition->prodId, false );

	for ( TransMap::Iter tt = expandToState->transMap; tt.lte(); tt++ ) {
		if ( alphSet.find( tt->key ) ) {
			trySetTime( tt->value, redCode, time );
	
			/* If the items token region is not recorded in the state, do it now. */
			addRegion( expandToState, tt->value, tt->key, 
					tt->value->noPreIgnore, tt->value->noPostIgnore );
		}
	}
}

bool regionVectHas( RegionVect &regVect, TokenRegion *region )
{
	for ( RegionVect::Iter trvi = regVect; trvi.lte(); trvi++ ) {
		if ( *trvi == region )
			return true;
	}
	return false;
}

void Compiler::addRegion( PdaState *tabState, PdaTrans *tabTrans,
		long pdaKey, bool noPreIgnore, bool noPostIgnore )
{
	LangEl *langEl = langElIndex[pdaKey];
	if ( langEl != 0 && langEl->type == LangEl::Term ) {
		TokenRegion *region = 0;

		/* If it is not the eof, then use the region associated 
		 * with the token definition. */
		if ( langEl->isCI ) {
			//cerr << "isCI" << endl;
			region = langEl->ciRegion->ciRegion;
		}
		else if ( !langEl->isEOF && langEl->tokenDef != 0 ) {
			region = langEl->tokenDef->tokenRegion;
		}

		if ( region != 0 ) {
			/* region. */
			TokenRegion *scanRegion = region;

			if ( langEl->noPreIgnore )
				scanRegion = region->tokenOnlyRegion;

			if ( !regionVectHas( tabState->regions, scanRegion ) ) {
				tabState->regions.append( scanRegion );
			}

			/* Pre-region of to state */
			PdaState *toState = tabTrans->toState;
			if ( !langEl->noPostIgnore && 
					region->ignoreOnlyRegion != 0 && 
					!regionVectHas( toState->preRegions, region->ignoreOnlyRegion ) )
			{
				toState->preRegions.append( region->ignoreOnlyRegion );
			}
		}
	}
}

#if 0
    orderState( tabState, prodState, time ):
        if not tabState.dotSet.find( prodState.dotID )
            tabState.dotSet.insert( prodState.dotID )
            tabTrans = tabState.findMatchingTransition( prodState.getTransition() )

            if tabTrans is NonTerminal:
                for production in tabTrans.nonTerm.prodList:
                    orderState( tabState, production.startState, time )

                    for all expandToState in tabTrans.expandToStates:
                        for all followTrans in expandToState.transList 
                            reduceAction = findAction( production.reduction )
                            if reduceAction.time is unset:
                                reduceAction.time = time++
                            end
                        end
                    end
                end
            end

            shiftAction = tabTrans.findAction( shift )
            if shiftAction.time is unset:
                shiftAction.time = time++
            end

            orderState( tabTrans.toState, prodTrans.toState, time )
        end
    end

    orderState( parseTable.startState, startProduction.startState, 1 )
#endif

void Compiler::pdaOrderProd( LangEl *rootEl, PdaState *tabState, 
	PdaState *srcState, Production *parentDef, long &time )
{
	assert( srcState->dotSet.length() == 1 );
	if ( tabState->dotSet2.find( srcState->dotSet[0] ) )
		return;
	tabState->dotSet2.insert( srcState->dotSet[0] );
	
	assert( srcState->transMap.length() == 0 || srcState->transMap.length() == 1 );

	if ( srcState->transMap.length() == 1 ) { 
		TransMap::Iter srcTrans = srcState->transMap;

		/* Find the equivalent state in the parser. */
		PdaTrans *tabTrans = tabState->findTrans( srcTrans->key );

		/* Recurse into the transition if it is a non-terminal. */
		LangEl *langEl = langElIndex[srcTrans->key];
		if ( langEl != 0 ) {
			if ( langEl->reduceFirst ) {
				/* Use a shortest match ordering for the contents of this
				 * nonterminal. Does follows for all productions first, then
				 * goes down the productions. */
				for ( LelDefList::Iter expDef = langEl->defList; expDef.lte(); expDef++ ) {
					pdaOrderFollow( rootEl, tabState, tabTrans, srcTrans->value, 
							parentDef, expDef, time );
				}
				for ( LelDefList::Iter expDef = langEl->defList; expDef.lte(); expDef++ )
					pdaOrderProd( rootEl, tabState, expDef->fsm->startState, expDef, time );
				
			}
			else {
				/* The default action ordering. For each prod, goes down the
				 * prod then sets the follow before going to the next prod. */
				for ( LelDefList::Iter expDef = langEl->defList; expDef.lte(); expDef++ ) {
					pdaOrderProd( rootEl, tabState, expDef->fsm->startState, expDef, time );

					pdaOrderFollow( rootEl, tabState, tabTrans, srcTrans->value, 
							parentDef, expDef, time );
				}
			}
		}

		trySetTime( tabTrans, SHIFT_CODE, time );

		/* Now possibly for the dup. */
		if ( langEl != 0 && langEl->termDup != 0 ) {
			PdaTrans *dupTrans = tabState->findTrans( langEl->termDup->id );
			trySetTime( dupTrans, SHIFT_CODE, time );
		}

		/* If the items token region is not recorded in the state, do it now. */
		addRegion( tabState, tabTrans, srcTrans->key, 
				srcTrans->value->noPreIgnore, srcTrans->value->noPostIgnore );

		/* Go over one in the production. */
		pdaOrderProd( rootEl, tabTrans->toState, 
				srcTrans->value->toState, parentDef, time );
	}
}

void Compiler::pdaActionOrder( PdaGraph *pdaGraph, LangElSet &parserEls )
{
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		assert( (state->stateBits & SB_ISMARKED) == 0 );

		/* Traverse the src state's transitions. */
		long last = 0;
		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			if ( ! trans.first() )
				assert( last < trans->key );
			last = trans->key;
		}
	}

	/* Compute the action orderings, record the max value. */
	long time = 1;
	for ( LangElSet::Iter pe = parserEls; pe.lte(); pe++ ) {
		PdaState *startState = (*pe)->rootDef->fsm->startState;
		pdaOrderProd( *pe, (*pe)->startState, startState, (*pe)->rootDef, time );

		/* Walk over the start lang el and set the time for shift of
		 * the eof action that completes the parse. */
		PdaTrans *overStart = (*pe)->startState->findTrans( (*pe)->id );
		PdaTrans *eofTrans = overStart->toState->findTrans( (*pe)->eofLel->id );
		eofTrans->actOrds[0] = time++;
	}

	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		if ( state->regions.length() == 0 ) {
			for ( TransMap::Iter tel = state->transMap; tel.lte(); tel++ ) {
				/* There are no regions and EOF leaves the state. Add the eof
				 * token region. */
				PdaTrans *trans = tel->value;
				LangEl *lel = langElIndex[trans->lowKey];
				if ( lel != 0 && lel->isEOF )
					state->regions.append( eofTokenRegion );
			}
		}
	}

	///* Warn about states with empty token region lists. */
	//for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
	//	if ( state->regions.length() == 0 ) {
	//		warning() << "state has an empty token region, state: " << 
	//			state->stateNum << endl;
	//	}
	//}

	/* Some actions may not have an ordering. I believe these to be actions
	 * that result in a parse error and they arise because the state tables
	 * are LALR(1) but the action ordering is LR(1). LALR(1) causes some
	 * reductions that lead nowhere. */
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		assert( CmpDotSet::compare( state->dotSet, state->dotSet2 ) == 0 );
		for ( TransMap::Iter tel = state->transMap; tel.lte(); tel++ ) {
			PdaTrans *trans = tel->value;
			/* Check every action has an ordering. */
			for ( ActDataList::Iter adl = trans->actOrds; adl.lte(); adl++ ) {
				if ( *adl == 0 )
					*adl = time++;
			}
		}
	}
}

void Compiler::advanceReductions( PdaGraph *pdaGraph )
{
	/* Loop all states. */
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		if ( !state->advanceReductions )
			continue;

		bool outHasShift = false;
		ReductionMap outReds;
		LongSet outCommits;
		for ( TransMap::Iter out = state->transMap; out.lte(); out++ ) {
			/* Get the transition from the trans el. */
			if ( out->value->isShift )
				outHasShift = true;
			outReds.insert( out->value->reductions );
			outCommits.insert( out->value->commits );
		}

		bool inHasShift = false;
		ReductionMap inReds;
		for ( PdaTransInList::Iter in = state->inRange; in.lte(); in++ ) {
			/* Get the transition from the trans el. */
			if ( in->isShift )
				inHasShift = true;
			inReds.insert( in->reductions );
		}

		if ( !outHasShift && outReds.length() == 1 && 
				inHasShift && inReds.length() == 0 )
		{
			//cerr << "moving reduction to shift" << endl;

			/* Move the reduction to all in transitions. */
			for ( PdaTransInList::Iter in = state->inRange; in.lte(); in++ ) {
				assert( in->actions.length() == 1 );
				assert( in->actions[0] == SHIFT_CODE );
				in->actions[0] = makeReduceCode( outReds[0].key, true );
				in->afterShiftCommits.insert( outCommits );
			}

			/* 
			 * Remove all transitions out of the state.
			 */

			/* Detach out range transitions. */
			for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
				pdaGraph->detachTrans( state, trans->value->toState, trans->value );
				delete trans->value;
			}
			state->transMap.empty();

			/* Redirect all the in transitions to the actionDestState. */
			pdaGraph->inTransMove( actionDestState, state );
		}
	}

	pdaGraph->removeUnreachableStates();
}

void Compiler::sortActions( PdaGraph *pdaGraph )
{
	/* Sort the actions. */
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		assert( CmpDotSet::compare( state->dotSet, state->dotSet2 ) == 0 );
		for ( TransMap::Iter tel = state->transMap; tel.lte(); tel++ ) {
			PdaTrans *trans = tel->value;

			/* Sort by the action ords. */
			ActDataList actions( trans->actions );
			ActDataList actOrds( trans->actOrds );
			ActDataList actPriors( trans->actPriors );
			trans->actions.empty();
			trans->actOrds.empty();
			trans->actPriors.empty();
			while ( actOrds.length() > 0 ) {
				int min = 0;
				for ( int i = 1; i < actOrds.length(); i++ ) {
					if ( actPriors[i] > actPriors[min] ||
							(actPriors[i] == actPriors[min] &&
							actOrds[i] < actOrds[min] ) )
					{
						min = i;
					}
				}
				trans->actions.append( actions[min] );
				trans->actOrds.append( actOrds[min] );
				trans->actPriors.append( actPriors[min] );
				actions.remove(min);
				actOrds.remove(min);
				actPriors.remove(min);
			}

			if ( branchPointInfo && trans->actions.length() > 1 ) {
				cerr << "info: branch point"
						<< " state: " << state->stateNum
						<< " trans: ";
				LangEl *lel = langElIndex[trans->lowKey];
				if ( lel == 0 )
					cerr << (char)trans->lowKey << endl;
				else
					cerr << lel->lit << endl;

				for ( ActDataList::Iter act = trans->actions; act.lte(); act++ ) {
					switch ( *act & 0x3 ) {
					case 1: 
						cerr << "    shift" << endl;
						break;
					case 2: 
						cerr << "    reduce " << 
								prodIdIndex[(*act >> 2)]->data << endl;
						break;
					case 3:
						cerr << "    shift-reduce" << endl;
						break;
					}
				}
			}

			/* Verify that shifts of nonterminals don't have any branch
			 * points or commits. */
			if ( trans->lowKey >= firstNonTermId ) {
				if ( trans->actions.length() != 1 || 
					(trans->actions[0] & 0x3) != 1 )
				{
					error() << "TRANS ON NONTERMINAL is something "
						"other than a shift" << endl;
				}
				if ( trans->commits.length() > 0 )
					error() << "TRANS ON NONTERMINAL has a commit" << endl;
			}

			/* TODO: Shift-reduces are optimizations. Verify that
			 * shift-reduces exist only if they don't entail a conflict. */
		}
	}
}

void Compiler::reduceActions( PdaGraph *pdaGraph )
{
	/* Reduce the actions. */
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( TransMap::Iter tel = state->transMap; tel.lte(); tel++ ) {
			PdaTrans *trans = tel->value;
			PdaActionSetEl *inSet;

			int commitLen = trans->commits.length() > 0 ?
				trans->commits[trans->commits.length()-1] : 0;

			if ( trans->afterShiftCommits.length() > 0 ) {
				int afterShiftCommit = trans->afterShiftCommits[
					trans->afterShiftCommits.length()-1];

				if ( commitLen > 0 && commitLen+1 > afterShiftCommit )
					commitLen = ( commitLen + 1 );
				else
					commitLen = afterShiftCommit;
			}
			else {
				commitLen = commitLen * -1;
			}
			
			//if ( commitLen != 0 ) {
			//	cerr << "FINAL ACTION COMMIT LEN: " << commitLen << endl;
			//}

			pdaGraph->actionSet.insert( ActionData( trans->toState->stateNum, 
					trans->actions, commitLen ), &inSet );
			trans->actionSetEl = inSet;
		}
	}
}

void Compiler::computeAdvanceReductions( LangEl *langEl, PdaGraph *pdaGraph )
{
	/* Get the entry into the graph and traverse over the root. The resulting
	 * state can have eof, nothing else can. */
	PdaState *overStart = pdaGraph->followFsm( 
			langEl->startState,
			langEl->rootDef->fsm );

	/* The graph must reduce to root all on it's own. It cannot depend on
	 * require EOF. */
	for ( PdaStateList::Iter st = pdaGraph->stateList; st.lte(); st++ ) {
		if ( st == overStart )
			continue;

		for ( TransMap::Iter tr = st->transMap; tr.lte(); tr++ ) {
			if ( tr->value->lowKey == langEl->eofLel->id )
				st->advanceReductions = true;
		}
	}
}

void Compiler::verifyParseStopGrammar( LangEl *langEl, PdaGraph *pdaGraph )
{
	/* Get the entry into the graph and traverse over the root. The resulting
	 * state can have eof, nothing else can. */
	PdaState *overStart = pdaGraph->followFsm( 
			langEl->startState,
			langEl->rootDef->fsm );

	/* The graph must reduce to root all on it's own. It cannot depend on
	 * require EOF. */
	for ( PdaStateList::Iter st = pdaGraph->stateList; st.lte(); st++ ) {
		if ( st == overStart )
			continue;

		for ( TransMap::Iter tr = st->transMap; tr.lte(); tr++ ) {
			if ( tr->value->lowKey == langEl->eofLel->id ) {
				/* This needs a better error message. Appears to be voodoo. */
				error() << "grammar is not usable with parse_stop" << endp;
			}
		}
	}
}

LangEl *Compiler::predOf( PdaTrans *trans, long action )
{
	LangEl *lel;
	if ( action == SHIFT_CODE )
		lel = langElIndex[trans->lowKey];
	else
		lel = prodIdIndex[action >> 2]->predOf;
	return lel;
}


bool Compiler::precedenceSwap( long action1, long action2, LangEl *l1, LangEl *l2 )
{
	bool swap = false;
	if ( l2->predValue > l1->predValue )
		swap = true;
	else if ( l1->predValue == l2->predValue ) {
		if ( l1->predType == PredLeft && action1 == SHIFT_CODE )
			swap = true;
		else if ( l1->predType == PredRight && action2 == SHIFT_CODE )
			swap = true;
	}
	return swap;
}

bool Compiler::precedenceRemoveBoth( LangEl *l1, LangEl *l2 )
{
	if ( l1->predValue == l2->predValue && l1->predType == PredNonassoc )
		return true;
	return false;
}

void Compiler::resolvePrecedence( PdaGraph *pdaGraph )
{
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		assert( CmpDotSet::compare( state->dotSet, state->dotSet2 ) == 0 );
		
		for ( long t = 0; t < state->transMap.length(); /* increment at end */ ) {
			PdaTrans *trans = state->transMap[t].value;

again:
			/* Find action with precedence. */
			for ( int i = 0; i < trans->actions.length(); i++ ) {
				LangEl *li = predOf( trans, trans->actions[i] );
					
				if ( li != 0 && li->predType != PredNone ) {
					/* Find another action with precedence. */
					for ( int j = i+1; j < trans->actions.length(); j++ ) {
						LangEl *lj = predOf( trans, trans->actions[j] );

						if ( lj != 0 && lj->predType != PredNone ) {
							/* Conflict to check. */
							bool swap = precedenceSwap( trans->actions[i], 
									trans->actions[j], li, lj );
							
							if ( swap ) {
								long t = trans->actions[i];
								trans->actions[i] = trans->actions[j];
								trans->actions[j] = t;
							}

							trans->actions.remove( j );
							if ( precedenceRemoveBoth( li, lj ) )
								trans->actions.remove( i );

							goto again;
						}
					}
				}
			}

			/* If there are still actions then move to the next one. If not,
			 * (due to nonassoc) then remove the transition. */
			if ( trans->actions.length() > 0 )
				t += 1;
			else
				state->transMap.vremove( t );
		}
	}
}

void Compiler::analyzeMachine( PdaGraph *pdaGraph, LangElSet &parserEls )
{
	pdaGraph->maxState = pdaGraph->stateList.length() - 1;
	pdaGraph->maxLelId = nextSymbolId - 1;
	pdaGraph->maxOffset = pdaGraph->stateList.length() * pdaGraph->maxLelId;

	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			if ( trans->value->isShift ) {
				trans->value->actions.append( SHIFT_CODE );
				trans->value->actPriors.append( trans->value->shiftPrior );
			}
			for ( ReductionMap::Iter red = trans->value->reductions; red.lte(); red++ ) {
				trans->value->actions.append( makeReduceCode( red->key, false ) );
				trans->value->actPriors.append( red->value );
			}
			trans->value->actOrds.appendDup( 0, trans->value->actions.length() );
		}
	}

	pdaActionOrder( pdaGraph, parserEls );
	sortActions( pdaGraph );
	resolvePrecedence( pdaGraph );

	/* Verify that any type we parse_stop can actually be parsed that way. */
	for ( LangElSet::Iter pe = parserEls; pe.lte(); pe++ ) {
		LangEl *lel = *pe;
		if ( lel->parseStop )
			computeAdvanceReductions(lel , pdaGraph);
	}

	advanceReductions( pdaGraph );
	pdaGraph->setStateNumbers();
	reduceActions( pdaGraph );

	/* Set the action ids. */
	int actionSetId = 0;
	for ( PdaActionSet::Iter asi = pdaGraph->actionSet; asi.lte(); asi++ )
		asi->key.id = actionSetId++;
	
	/* Get the max index. */
	pdaGraph->maxIndex = actionSetId - 1;

	/* Compute the max prod length. */
	pdaGraph->maxProdLen = 0;
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		if ( (unsigned)prod->fsmLength > pdaGraph->maxProdLen )
			pdaGraph->maxProdLen = prod->fsmLength;
	}

	/* Asserts that any transition with a nonterminal has a single action
	 * which is either a shift or a shift-reduce. */
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			LangEl *langEl = langElIndex[trans->value->lowKey];
			if ( langEl != 0 && langEl->type == LangEl::NonTerm ) {
				assert( trans->value->actions.length() == 1 );
				assert( trans->value->actions[0] == SHIFT_CODE ||
					(trans->value->actions[0] & 0x3) == SHIFT_REDUCE_CODE );
			}
		}
	}

	/* Assert that shift reduces always appear on their own. */
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			for ( ActDataList::Iter act = trans->value->actions; act.lte(); act++ ) {
				if ( (*act & 0x3) == SHIFT_REDUCE_CODE )
					assert( trans->value->actions.length() == 1 );
			}
		}
	}

	/* Verify that any type we parse_stop can actually be parsed that way. */
	for ( LangElSet::Iter pe = parserEls; pe.lte(); pe++ ) {
		LangEl *lel = *pe;
		if ( lel->parseStop )
			verifyParseStopGrammar(lel , pdaGraph);
	}
}

void Compiler::wrapNonTerminals()
{
	/* Make a language element that will be used to make the root productions.
	 * These are used for making parsers rooted at any production (including
	 * the start symbol). */
	rootLangEl = declareLangEl( this, rootNamespace, "_root", LangEl::NonTerm );

	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		/* Make a single production used when the lel is a root. */
		ProdElList *prodElList = makeProdElList( lel );
		lel->rootDef = Production::cons( InputLoc(), rootLangEl, 
				prodElList, false, 0,
				prodList.length(), rootLangEl->defList.length() );
		prodList.append( lel->rootDef );
		rootLangEl->defList.append( lel->rootDef );

		/* First resolve. */
		for ( ProdElList::Iter fact = *prodElList; fact.lte(); fact++ )
			resolveFactor( fact );
	}
}

bool Compiler::makeNonTermFirstSetProd( Production *prod, PdaState *state )
{
	bool modified = false;
	for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
		if ( trans->key >= firstNonTermId ) {
			long *inserted = prod->nonTermFirstSet.insert( trans->key );
			if ( inserted != 0 )
				modified = true;

			bool hasEpsilon = false;
			LangEl *lel = langElIndex[trans->key];
			for ( LelDefList::Iter ldef = lel->defList; ldef.lte(); ldef++ ) {
				for ( ProdIdSet::Iter pid = ldef->nonTermFirstSet; 
						pid.lte(); pid++ )
				{
					if ( *pid == -1 )
						hasEpsilon = true;
					else {
						long *inserted = prod->nonTermFirstSet.insert( *pid );
						if ( inserted != 0 )
							modified = true;
					}
				}
			}

			if ( hasEpsilon ) {
				if ( trans->value->toState->isFinState() ) {
					long *inserted = prod->nonTermFirstSet.insert( -1 );
					if ( inserted != 0 )
						modified = true;
				}

				bool lmod = makeNonTermFirstSetProd( prod, trans->value->toState );
				if ( lmod )
					modified = true;
			}
		}
	}
	return modified;
}


void Compiler::makeNonTermFirstSets()
{
	bool modified = true;
	while ( modified ) {
		modified = false;
		for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
			if ( prod->fsm->startState->isFinState() ) {
				long *inserted = prod->nonTermFirstSet.insert( -1 );
				if ( inserted != 0 )
					modified = true;
			}

			bool lmod = makeNonTermFirstSetProd( prod, prod->fsm->startState );
			if ( lmod )
				modified = true;
		}
	}

	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		if ( prod->nonTermFirstSet.find( prod->prodName->id ) )
			prod->isLeftRec = true;
	}
}

void Compiler::printNonTermFirstSets()
{
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		cerr << prod->data << ": ";
		for ( ProdIdSet::Iter pid = prod->nonTermFirstSet; pid.lte(); pid++ )
		{
			if ( *pid < 0 )
				cerr << " <EPSILON>";
			else {
				LangEl *lel = langElIndex[*pid];
				cerr << " " << lel->name;
			}
		}
		cerr << endl;

		if ( prod->isLeftRec )
			cerr << "PROD IS LEFT REC: " << prod->data << endl;
	}
}

bool Compiler::makeFirstSetProd( Production *prod, PdaState *state )
{
	bool modified = false;
	for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
		if ( trans->key < firstNonTermId ) {
			long *inserted = prod->firstSet.insert( trans->key );
			if ( inserted != 0 )
				modified = true;
		}
		else {
			long *inserted = prod->firstSet.insert( trans->key );
			if ( inserted != 0 )
				modified = true;

			LangEl *klangEl = langElIndex[trans->key];
			if ( klangEl != 0 && klangEl->termDup != 0 ) {
				long *inserted2 = prod->firstSet.insert( klangEl->termDup->id );
				if ( inserted2 != 0 )
					modified = true;
			}

			bool hasEpsilon = false;
			LangEl *lel = langElIndex[trans->key];
			for ( LelDefList::Iter ldef = lel->defList; ldef.lte(); ldef++ ) {
				for ( ProdIdSet::Iter pid = ldef->firstSet; 
						pid.lte(); pid++ )
				{
					if ( *pid == -1 )
						hasEpsilon = true;
					else {
						long *inserted = prod->firstSet.insert( *pid );
						if ( inserted != 0 )
							modified = true;
					}
				}
			}

			if ( hasEpsilon ) {
				if ( trans->value->toState->isFinState() ) {
					long *inserted = prod->firstSet.insert( -1 );
					if ( inserted != 0 )
						modified = true;
				}

				bool lmod = makeFirstSetProd( prod, trans->value->toState );
				if ( lmod )
					modified = true;
			}
		}
	}
	return modified;
}


void Compiler::makeFirstSets()
{
	bool modified = true;
	while ( modified ) {
		modified = false;
		for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
			if ( prod->fsm->startState->isFinState() ) {
				long *inserted = prod->firstSet.insert( -1 );
				if ( inserted != 0 )
					modified = true;
			}

			bool lmod = makeFirstSetProd( prod, prod->fsm->startState );
			if ( lmod )
				modified = true;
		}
	}
}

void Compiler::printFirstSets()
{
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		cerr << prod->data << ": ";
		for ( ProdIdSet::Iter pid = prod->firstSet; pid.lte(); pid++ )
		{
			if ( *pid < 0 )
				cerr << " <EPSILON>";
			else {
				LangEl *lel = langElIndex[*pid];
				if ( lel != 0 ) 
					cerr << endl << "    " << lel->name;
				else
					cerr << endl << "    " << *pid;
			}
		}
		cerr << endl;
	}
}

void Compiler::insertUniqueEmptyProductions()
{
	int limit = prodList.length();
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		if ( prod->prodId == limit )
			break;

		/* Get a language element. */
		char name[20];
		sprintf(name, "U%li", prodList.length());
		LangEl *prodName = addLangEl( this, rootNamespace, name, LangEl::NonTerm );
		Production *newDef = Production::cons( InputLoc(), prodName, 
				0, false, 0, prodList.length(), prodName->defList.length() );
		prodName->defList.append( newDef );
		prodList.append( newDef );

		prod->uniqueEmptyLeader = prodName;
	}
}

void Compiler::makeRuntimeData()
{
	long count = 0;

	/*
	 * ProdLengths
	 * ProdLhsIs
	 * ProdNames
	 * ProdCodeBlocks
	 * ProdCodeBlockLens
	 */

	runtimeData->frameInfo = new FrameInfo[nextFrameId];
	runtimeData->numFrames = nextFrameId;
	memset( runtimeData->frameInfo, 0, sizeof(FrameInfo) * nextFrameId );

	/*
	 * Init code block.
	 */
	if ( rootCodeBlock == 0 ) {
		runtimeData->rootCode = 0;
		runtimeData->rootCodeLen = 0;
		runtimeData->rootFrameId = 0;
	}
	else {
		runtimeData->rootCode = rootCodeBlock->codeWC.data;
		runtimeData->rootCodeLen = rootCodeBlock->codeWC.length();
		runtimeData->rootFrameId = rootCodeBlock->frameId;
	}

	runtimeData->frameInfo[rootCodeBlock->frameId].codeWV = 0;
	runtimeData->frameInfo[rootCodeBlock->frameId].codeLenWV = 0;
	runtimeData->frameInfo[rootCodeBlock->frameId].trees = rootCodeBlock->trees.data;
	runtimeData->frameInfo[rootCodeBlock->frameId].treesLen = rootCodeBlock->trees.length();
	runtimeData->frameInfo[rootCodeBlock->frameId].frameSize = rootLocalFrame->size();
	runtimeData->frameInfo[rootCodeBlock->frameId].argSize = 0;

	/*
	 * prodInfo
	 */
	count = prodList.length();
	runtimeData->prodInfo = new ProdInfo[count];
	runtimeData->numProds = count;

	count = 0;
	for ( DefList::Iter prod = prodList; prod.lte(); prod++ ) {
		runtimeData->prodInfo[count].lhsId = prod->prodName->id;
		runtimeData->prodInfo[count].prodNum = prod->prodNum;
		runtimeData->prodInfo[count].length = prod->fsmLength;
		runtimeData->prodInfo[count].name = prod->data;
		runtimeData->prodInfo[count].frameId = -1;

		CodeBlock *block = prod->redBlock;
		if ( block != 0 ) {
			runtimeData->prodInfo[count].frameId = block->frameId;
			runtimeData->frameInfo[block->frameId].codeWV = block->codeWV.data;
			runtimeData->frameInfo[block->frameId].codeLenWV = block->codeWV.length();

			runtimeData->frameInfo[block->frameId].trees = block->trees.data;
			runtimeData->frameInfo[block->frameId].treesLen = block->trees.length();

			runtimeData->frameInfo[block->frameId].frameSize = block->localFrame->size();
			runtimeData->frameInfo[block->frameId].argSize = 0;
		}

		runtimeData->prodInfo[count].lhsUpref = true;
		runtimeData->prodInfo[count].copy = prod->copy.data;
		runtimeData->prodInfo[count].copyLen = prod->copy.length() / 2;
		count += 1;
	}

	/*
	 * regionInfo
	 */
	runtimeData->numRegions = regionList.length()+1;
	runtimeData->regionInfo = new RegionInfo[runtimeData->numRegions];
	memset( runtimeData->regionInfo, 0, sizeof(RegionInfo) * runtimeData->numRegions );

	runtimeData->regionInfo[0].defaultToken = -1;
	for ( RegionList::Iter reg = regionList; reg.lte(); reg++ ) {
		long regId = reg->id+1;
		runtimeData->regionInfo[regId].defaultToken =
			reg->defaultTokenDef == 0 ? -1 : reg->defaultTokenDef->tdLangEl->id;
		runtimeData->regionInfo[regId].eofFrameId = -1;
		runtimeData->regionInfo[regId].isIgnoreOnly = reg->isIgnoreOnly;
		runtimeData->regionInfo[regId].isCiOnly = reg->isCiOnly;
		runtimeData->regionInfo[regId].ciLelId = reg->isCiOnly ? reg->derivedFrom->ciLel->id : 0;

		CodeBlock *block = reg->preEofBlock;
		if ( block != 0 ) {
			runtimeData->regionInfo[regId].eofFrameId = block->frameId;
			runtimeData->frameInfo[block->frameId].codeWV = block->codeWV.data;
			runtimeData->frameInfo[block->frameId].codeLenWV = block->codeWV.length();

			runtimeData->frameInfo[block->frameId].trees = block->trees.data;
			runtimeData->frameInfo[block->frameId].treesLen = block->trees.length();

			runtimeData->frameInfo[block->frameId].frameSize = block->localFrame->size();
			runtimeData->frameInfo[block->frameId].argSize = 0;
		}
	}

	/*
	 * lelInfo
	 */

	count = nextSymbolId;
	runtimeData->lelInfo = new LangElInfo[count];
	runtimeData->numLangEls = count;
	memset( runtimeData->lelInfo, 0, sizeof(LangElInfo)*count );

	for ( int i = 0; i < nextSymbolId; i++ ) {
		LangEl *lel = langElIndex[i];
		if ( lel != 0 ) {
			runtimeData->lelInfo[i].name = lel->fullLit;
			runtimeData->lelInfo[i].xmlTag = lel->xmlTag;
			runtimeData->lelInfo[i].repeat = lel->isRepeat;
			runtimeData->lelInfo[i].list = lel->isList;
			runtimeData->lelInfo[i].literal = lel->isLiteral;
			runtimeData->lelInfo[i].ignore = lel->ignore;
			runtimeData->lelInfo[i].frameId = -1;

			CodeBlock *block = lel->transBlock;
			if ( block != 0 ) {
				runtimeData->lelInfo[i].frameId = block->frameId;
				runtimeData->frameInfo[block->frameId].codeWV = block->codeWV.data;
				runtimeData->frameInfo[block->frameId].codeLenWV = block->codeWV.length();

				runtimeData->frameInfo[block->frameId].trees = block->trees.data;
				runtimeData->frameInfo[block->frameId].treesLen = block->trees.length();

				runtimeData->frameInfo[block->frameId].frameSize = block->localFrame->size();
				runtimeData->frameInfo[block->frameId].argSize = 0;
			}

			
			runtimeData->lelInfo[i].objectTypeId = 
					lel->objectDef == 0 ? 0 : lel->objectDef->id;
			runtimeData->lelInfo[i].ofiOffset = lel->ofiOffset;
			runtimeData->lelInfo[i].objectLength = 
					( lel->objectDef == 0 || lel->objectDef == tokenObj ) ? 0 : 
					lel->objectDef->size();

//			runtimeData->lelInfo[i].contextTypeId = 0;
//					lel->context == 0 ? 0 : lel->context->contextObjDef->id;
//			runtimeData->lelInfo[i].contextLength = 0; //lel->context == 0 ? 0 :
//					lel->context->contextObjDef->size();
//			if ( lel->context != 0 ) {
//				cout << "type: " << runtimeData->lelInfo[i].contextTypeId << " length: " << 
//					runtimeData->lelInfo[i].contextLength << endl;
//			}

			runtimeData->lelInfo[i].termDupId = lel->termDup == 0 ? 0 : lel->termDup->id;
			runtimeData->lelInfo[i].genericId = lel->generic == 0 ? 0 : lel->generic->id;

			if ( lel->tokenDef != 0 && lel->tokenDef->join != 0 && 
					lel->tokenDef->join->context != 0 )
				runtimeData->lelInfo[i].markId = lel->tokenDef->join->mark->markId;
			else
				runtimeData->lelInfo[i].markId = -1;

			runtimeData->lelInfo[i].numCaptureAttr = 0;
		}
		else {
			memset(&runtimeData->lelInfo[i], 0, sizeof(LangElInfo) );
			runtimeData->lelInfo[i].name = "__UNUSED";
			runtimeData->lelInfo[i].xmlTag = "__UNUSED";
			runtimeData->lelInfo[i].frameId = -1;
		}
	}

	/*
	 * FunctionInfo
	 */
	count = functionList.length();

	runtimeData->functionInfo = new FunctionInfo[count];
	runtimeData->numFunctions = count;
	memset( runtimeData->functionInfo, 0, sizeof(FunctionInfo)*count );
	for ( FunctionList::Iter func = functionList; func.lte(); func++ ) {
		runtimeData->functionInfo[func->funcId].name = func->name;
		runtimeData->functionInfo[func->funcId].frameId = -1;

		CodeBlock *block = func->codeBlock;
		if ( block != 0 ) {
			runtimeData->functionInfo[func->funcId].frameId = block->frameId;

			runtimeData->frameInfo[block->frameId].codeWV = block->codeWV.data;
			runtimeData->frameInfo[block->frameId].codeLenWV = block->codeWV.length();

			runtimeData->frameInfo[block->frameId].codeWC = block->codeWC.data;
			runtimeData->frameInfo[block->frameId].codeLenWC = block->codeWC.length();

			runtimeData->frameInfo[block->frameId].trees = block->trees.data;
			runtimeData->frameInfo[block->frameId].treesLen = block->trees.length();

			runtimeData->frameInfo[block->frameId].frameSize = func->localFrame->size();
			runtimeData->frameInfo[block->frameId].argSize = func->paramListSize;
		}

		runtimeData->functionInfo[func->funcId].frameSize = func->localFrame->size();
		runtimeData->functionInfo[func->funcId].argSize = func->paramListSize;
	}

	/*
	 * PatConsInfo
	 */

	/* Filled in later after patterns are parsed. */
	runtimeData->patReplInfo = new PatConsInfo[nextPatConsId];
	memset( runtimeData->patReplInfo, 0, sizeof(PatConsInfo) * nextPatConsId );
	runtimeData->numPatterns = nextPatConsId;
	runtimeData->patReplNodes = 0;
	runtimeData->numPatternNodes = 0;

	
	/*
	 * GenericInfo
	 */
	count = 1;
	for ( NamespaceList::Iter nspace = namespaceList; nspace.lte(); nspace++ )
		count += nspace->genericList.length();
	assert( count == nextGenericId );

	runtimeData->genericInfo = new GenericInfo[count];
	runtimeData->numGenerics = count;
	memset( &runtimeData->genericInfo[0], 0, sizeof(GenericInfo) );
	for ( NamespaceList::Iter nspace = namespaceList; nspace.lte(); nspace++ ) {
		for ( GenericList::Iter gen = nspace->genericList; gen.lte(); gen++ ) {
			runtimeData->genericInfo[gen->id].type = gen->typeId;
			runtimeData->genericInfo[gen->id].typeArg = gen->utArg->typeId;
			runtimeData->genericInfo[gen->id].keyType = gen->keyUT != 0 ? 
					gen->keyUT->typeId : 0;
			runtimeData->genericInfo[gen->id].keyOffset = 0;
			runtimeData->genericInfo[gen->id].langElId = gen->langEl->id;
			runtimeData->genericInfo[gen->id].parserId = gen->utArg->langEl->parserId;
		}
	}

	runtimeData->argvGenericId = argvTypeRef->generic->id;

	/*
	 * Literals
	 */
	runtimeData->numLiterals = literalStrings.length();
	runtimeData->litdata = new const char *[literalStrings.length()];
	runtimeData->litlen = new long [literalStrings.length()];
	runtimeData->literals = 0;
	for ( StringMap::Iter el = literalStrings; el.lte(); el++ ) {
		/* Data. */
		char *data = new char[el->key.length()+1];
		memcpy( data, el->key.data, el->key.length() );
		data[el->key.length()] = 0;
		runtimeData->litdata[el->value] = data;

		/* Length. */
		runtimeData->litlen[el->value] = el->key.length();
	}

	/* Captured attributes. Loop over tokens and count first. */
	long numCapturedAttr = 0;
//	for ( RegionList::Iter reg = regionList; reg.lte(); reg++ ) {
//		for ( TokenDefListReg::Iter td = reg->tokenDefList; td.lte(); td++ )
//			numCapturedAttr += td->reCaptureVect.length();
//	}
	runtimeData->captureAttr = new CaptureAttr[numCapturedAttr];
	runtimeData->numCapturedAttr = numCapturedAttr;
	memset( runtimeData->captureAttr, 0, sizeof( CaptureAttr ) * numCapturedAttr );

	count = 0;
//	for ( RegionList::Iter reg = regionList; reg.lte(); reg++ ) {
//		for ( TokenDefListReg::Iter td = reg->tokenDefList; td.lte(); td++ ) {
//			runtimeData->lelInfo[td->token->id].captureAttr = count;
//			runtimeData->lelInfo[td->token->id].numCaptureAttr = td->reCaptureVect.length();
//			for ( ReCaptureVect::Iter c = td->reCaptureVect; c.lte(); c++ ) {
//				runtimeData->captureAttr[count].mark_enter = c->markEnter->markId;
//				runtimeData->captureAttr[count].mark_leave = c->markLeave->markId;
//				runtimeData->captureAttr[count].offset = c->objField->offset;
//
//				count += 1;
//			}
//		}
//	}

	runtimeData->fsmTables = fsmTables;
	runtimeData->pdaTables = pdaTables;

	/* FIXME: need a parser descriptor. */
	runtimeData->startStates = new int[nextParserId];
	runtimeData->eofLelIds = new int[nextParserId];
	runtimeData->parserLelIds = new int[nextParserId];
	runtimeData->numParsers = nextParserId;
	for ( LelList::Iter lel = langEls; lel.lte(); lel++ ) {
		if ( lel->parserId >= 0 ) {
			runtimeData->startStates[lel->parserId] = lel->startState->stateNum;
			runtimeData->eofLelIds[lel->parserId] = lel->eofLel->id;
			runtimeData->parserLelIds[lel->parserId] = lel->id;
		}
	}

	runtimeData->globalSize = globalObjectDef->size();

	/*
	 * firstNonTermId
	 */
	runtimeData->firstNonTermId = firstNonTermId;

	/* Special trees. */
	runtimeData->integerId = intLangEl->id;
	runtimeData->stringId = strLangEl->id;
	runtimeData->anyId = anyLangEl->id;
	runtimeData->eofId = 0; //eofLangEl->id;
	runtimeData->noTokenId = noTokenLangEl->id;
}

/* Borrow alg->state for mapsTo. */
void countNodes( Program *prg, int &count, ParseTree *parseTree, Kid *kid )
{
	if ( kid != 0 ) {
		count += 1;

		/* Should't have to recurse here. */
		Tree *ignoreList = treeLeftIgnore( prg, kid->tree );
		if ( ignoreList != 0 ) {
			Kid *ignore = ignoreList->child;
			while ( ignore != 0 ) {
				count += 1;
				ignore = ignore->next;
			}
		}

		ignoreList = treeRightIgnore( prg, kid->tree );
		if ( ignoreList != 0 ) {
			Kid *ignore = ignoreList->child;
			while ( ignore != 0 ) {
				count += 1;
				ignore = ignore->next;
			}
		}
		
		//count += prg->rtd->lelInfo[kid->tree->id].numCaptureAttr;

		if ( !( parseTree->flags & PF_NAMED ) && 
				!( parseTree->flags & PF_ARTIFICIAL ) && 
				treeChild( prg, kid->tree ) != 0 )
		{
			countNodes( prg, count, parseTree->child, treeChild( prg, kid->tree ) );
		}
		countNodes( prg, count, parseTree->next, kid->next );
	}
}

void fillNodes( Program *prg, int &nextAvail, Bindings *bindings, long &bindId, 
		PatConsNode *nodes, ParseTree *parseTree, Kid *kid, int ind )
{
	if ( kid != 0 ) {
		PatConsNode &node = nodes[ind];

		Kid *child = 
			!( parseTree->flags & PF_NAMED ) && 
			!( parseTree->flags & PF_ARTIFICIAL ) && 
			treeChild( prg, kid->tree ) != 0 
			?
			treeChild( prg, kid->tree ) : 0;

		ParseTree *ptChild =
			!( parseTree->flags & PF_NAMED ) && 
			!( parseTree->flags & PF_ARTIFICIAL ) && 
			treeChild( prg, kid->tree ) != 0 
			?
			parseTree->child : 0;

		/* Set up the fields. */
		node.id = kid->tree->id;
		node.prodNum = kid->tree->prodNum;
		node.length = stringLength( kid->tree->tokdata );
		node.data = stringData( kid->tree->tokdata );

		/* Ignore items. */
		Tree *ignoreList = treeLeftIgnore( prg, kid->tree );
		Kid *ignore = ignoreList == 0 ? 0 : ignoreList->child;
		node.leftIgnore = ignore == 0 ? -1 : nextAvail;

		while ( ignore != 0 ) {
			PatConsNode &node = nodes[nextAvail++];

			memset( &node, 0, sizeof(PatConsNode) );
			node.id = ignore->tree->id;
			node.prodNum = ignore->tree->prodNum;
			node.next = ignore->next == 0 ? -1 : nextAvail;
				
			node.length = stringLength( ignore->tree->tokdata );
			node.data = stringData( ignore->tree->tokdata );

			ignore = ignore->next;
		}

		/* Ignore items. */
		ignoreList = treeRightIgnore( prg, kid->tree );
		ignore = ignoreList == 0 ? 0 : ignoreList->child;
		node.rightIgnore = ignore == 0 ? -1 : nextAvail;

		while ( ignore != 0 ) {
			PatConsNode &node = nodes[nextAvail++];

			memset( &node, 0, sizeof(PatConsNode) );
			node.id = ignore->tree->id;
			node.prodNum = ignore->tree->prodNum;
			node.next = ignore->next == 0 ? -1 : nextAvail;
				
			node.length = stringLength( ignore->tree->tokdata );
			node.data = stringData( ignore->tree->tokdata );

			ignore = ignore->next;
		}

		///* The captured attributes. */
		//for ( int i = 0; i < prg->rtd->lelInfo[kid->tree->id].numCaptureAttr; i++ ) {
		//	CaptureAttr *cap = prg->rtd->captureAttr + 
		//			prg->rtd->lelInfo[kid->tree->id].captureAttr + i;
		//
		//	Tree *attr = getAttr( kid->tree, cap->offset );
		//
		//	PatConsNode &node = nodes[nextAvail++];
		//	memset( &node, 0, sizeof(PatConsNode) );
		//
		//	node.id = attr->id;
		//	node.prodNum = attr->prodNum;
		//	node.length = stringLength( attr->tokdata );
		//	node.data = stringData( attr->tokdata );
		//}

		node.stop = parseTree->flags & PF_TERM_DUP;

		node.child = child == 0 ? -1 : nextAvail++; 

		/* Recurse. */
		fillNodes( prg, nextAvail, bindings, bindId, nodes, ptChild, child, node.child );

		/* Since the parser is bottom up the bindings are in a bottom up
		 * traversal order. Check after recursing. */
		node.bindId = 0;
		if ( bindId < bindings->length() && bindings->data[bindId] == parseTree ) {
			/* Remember that binding ids are indexed from one. */
			node.bindId = bindId++;

			//cout << "binding match in " << __PRETTY_FUNCTION__ << endl;
			//cout << "bindId: " << node.bindId << endl;
		}

		node.next = kid->next == 0 ? -1 : nextAvail++; 

		/* Move to the next child. */
		fillNodes( prg, nextAvail, bindings, bindId, nodes, parseTree->next, kid->next, node.next );
	}
}

void Compiler::fillInPatterns( Program *prg )
{
	/*
	 * patReplNodes
	 */

	/* Count is referenced and computed by mapNode. */
	int count = 0;
	for ( PatList::Iter pat = patternList; pat.lte(); pat++ ) {
		countNodes( prg, count, 
				pat->pdaRun->stackTop->next,
				pat->pdaRun->stackTop->next->shadow );
	}

	for ( ConsList::Iter repl = replList; repl.lte(); repl++ ) {
		countNodes( prg, count, 
				repl->pdaRun->stackTop->next,
				repl->pdaRun->stackTop->next->shadow );
	}
	
	runtimeData->patReplNodes = new PatConsNode[count];
	runtimeData->numPatternNodes = count;

	int nextAvail = 0;

	for ( PatList::Iter pat = patternList; pat.lte(); pat++ ) {
		int ind = nextAvail++;
		runtimeData->patReplInfo[pat->patRepId].offset = ind;

		/* BindIds are indexed base one. */
		runtimeData->patReplInfo[pat->patRepId].numBindings = 
				pat->pdaRun->bindings->length() - 1;

		/* Init the bind */
		long bindId = 1;
		fillNodes( prg, nextAvail, pat->pdaRun->bindings, bindId,
				runtimeData->patReplNodes, 
				pat->pdaRun->stackTop->next, 
				pat->pdaRun->stackTop->next->shadow, 
				ind );
	}

	for ( ConsList::Iter repl = replList; repl.lte(); repl++ ) {
		int ind = nextAvail++;
		runtimeData->patReplInfo[repl->patRepId].offset = ind;

		/* BindIds are indexed base one. */
		runtimeData->patReplInfo[repl->patRepId].numBindings = 
				repl->pdaRun->bindings->length() - 1;

		long bindId = 1;
		fillNodes( prg, nextAvail, repl->pdaRun->bindings, bindId,
				runtimeData->patReplNodes, 
				repl->pdaRun->stackTop->next,
				repl->pdaRun->stackTop->next->shadow, 
				ind );
	}

	assert( nextAvail == count );
}


int Compiler::findIndexOff( PdaTables *pdaTables, PdaGraph *pdaGraph, PdaState *state, int &curLen )
{
	for ( int start = 0; start < curLen;  ) {
		int offset = start;
		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			if ( pdaTables->owners[offset] != -1 )
				goto next_start;

			offset++;
			if ( ! trans.last() ) {
				TransMap::Iter next = trans.next();
				offset += next->key - trans->key - 1;
			}
		}

		/* Got though the whole list without a conflict. */
		return start;

next_start:
		start++;
	}

	return curLen;
}

struct CmpSpan
{
	static int compare( PdaState *state1, PdaState *state2 )
	{
		int dist1 = 0, dist2 = 0;

		if ( state1->transMap.length() > 0 ) {
			TransMap::Iter first1 = state1->transMap.first();
			TransMap::Iter last1 = state1->transMap.last();
			dist1 = last1->key - first1->key;
		}

		if ( state2->transMap.length() > 0 ) {
			TransMap::Iter first2 = state2->transMap.first();
			TransMap::Iter last2 = state2->transMap.last();
			dist2 = last2->key - first2->key;
		}

		if ( dist1 < dist2 )
			return 1;
		else if ( dist2 < dist1 )
			return -1;
		return 0;
	}
};

PdaGraph *Compiler::makePdaGraph( LangElSet &parserEls )
{
	//for ( DefList::Iter prod = prodList; prod.lte(); prod++ )
	//	cerr << prod->prodId << " " << prod->data << endl;

	PdaGraph *pdaGraph = new PdaGraph();
	lalr1GenerateParser( pdaGraph, parserEls );
	pdaGraph->setStateNumbers();
	analyzeMachine( pdaGraph, parserEls );

	//cerr << "NUMBER OF STATES: " << pdaGraph->stateList.length() << endl;

	return pdaGraph;
}

PdaTables *Compiler::makePdaTables( PdaGraph *pdaGraph )
{
	int count, pos;
	PdaTables *pdaTables = new PdaTables;

	/*
	 * Counting max indices.
	 */
	count = 0;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			count++;
			if ( ! trans.last() ) {
				TransMap::Iter next = trans.next();
				count +=  next->key - trans->key - 1;
			}
		}
	}


	/* Allocate indicies and owners. */
	pdaTables->numIndicies = count;
	pdaTables->indicies = new int[count];
	pdaTables->owners = new int[count];
	for ( long i = 0; i < count; i++ ) {
		pdaTables->indicies[i] = -1;
		pdaTables->owners[i] = -1;
	}

	/* Allocate offsets. */
	int numStates = pdaGraph->stateList.length(); 
	pdaTables->offsets = new unsigned int[numStates];
	pdaTables->numStates = numStates;

	/* Place transitions into indicies/owners */
	PdaState **states = new PdaState*[numStates];
	long ds = 0;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ )
		states[ds++] = state;

	/* Sorting baseded on span length. Gives an improvement, but incures a
	 * cost. Off for now. */
	//MergeSort< PdaState*, CmpSpan > mergeSort;
	//mergeSort.sort( states, numStates );
	
	int indLen = 0;
	for ( int s = 0; s < numStates; s++ ) {
		PdaState *state = states[s];

		int indOff = findIndexOff( pdaTables, pdaGraph, state, indLen );
		pdaTables->offsets[state->stateNum] = indOff;

		for ( TransMap::Iter trans = state->transMap; trans.lte(); trans++ ) {
			pdaTables->indicies[indOff] = trans->value->actionSetEl->key.id;
			pdaTables->owners[indOff] = state->stateNum;
			indOff++;

			if ( ! trans.last() ) {
				TransMap::Iter next = trans.next();
				indOff += next->key - trans->key - 1;
			}
		}

		if ( indOff > indLen )
			indLen = indOff;
	}

	/* We allocated the max, but cmpression gives us less. */
	pdaTables->numIndicies = indLen;
	delete[] states;
	

	/*
	 * Keys
	 */
	count = pdaGraph->stateList.length() * 2;;
	pdaTables->keys = new int[count];
	pdaTables->numKeys = count;

	count = 0;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		if ( state->transMap.length() == 0 ) {
			pdaTables->keys[count+0] = 0;
			pdaTables->keys[count+1] = 0;
		}
		else {
			TransMap::Iter first = state->transMap.first();
			TransMap::Iter last = state->transMap.last();
			pdaTables->keys[count+0] = first->key;
			pdaTables->keys[count+1] = last->key;
		}
		count += 2;
	}

	/*
	 * Targs
	 */
	count = pdaGraph->actionSet.length();
	pdaTables->targs = new unsigned int[count];
	pdaTables->numTargs = count;

	count = 0;
	for ( PdaActionSet::Iter asi = pdaGraph->actionSet; asi.lte(); asi++ )
		pdaTables->targs[count++] = asi->key.targ;

	/* 
	 * ActInds
	 */
	count = pdaGraph->actionSet.length();
	pdaTables->actInds = new unsigned int[count];
	pdaTables->numActInds = count;

	count = pos = 0;
	for ( PdaActionSet::Iter asi = pdaGraph->actionSet; asi.lte(); asi++ ) {
		pdaTables->actInds[count++] = pos;
		pos += asi->key.actions.length() + 1;
	}

	/*
	 * Actions
	 */
	count = 0;
	for ( PdaActionSet::Iter asi = pdaGraph->actionSet; asi.lte(); asi++ )
		count += asi->key.actions.length() + 1;

	pdaTables->actions = new unsigned int[count];
	pdaTables->numActions = count;

	count = 0;
	for ( PdaActionSet::Iter asi = pdaGraph->actionSet; asi.lte(); asi++ ) {
		for ( ActDataList::Iter ali = asi->key.actions; ali.lte(); ali++ )
			pdaTables->actions[count++] = *ali;

		pdaTables->actions[count++] = 0;
	}

	/*
	 * CommitLen
	 */
	count = pdaGraph->actionSet.length();
	pdaTables->commitLen = new int[count];
	pdaTables->numCommitLen = count;

	count = 0;
	for ( PdaActionSet::Iter asi = pdaGraph->actionSet; asi.lte(); asi++ )
		pdaTables->commitLen[count++] = asi->key.commitLen;
	
	/*
	 * tokenRegionInds. Start at one so region index 0 is null (unset).
	 */
	count = 0;
	pos = 1;
	pdaTables->tokenRegionInds = new int[pdaTables->numStates];
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		pdaTables->tokenRegionInds[count++] = pos;
		pos += state->regions.length() + 1;
	}


	/*
	 * tokenRegions. Build in a null at the beginning.
	 */

	count = 1;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ )
		count += state->regions.length() + 1;

	pdaTables->numRegionItems = count;
	pdaTables->tokenRegions = new int[pdaTables->numRegionItems];

	count = 0;
	pdaTables->tokenRegions[count++] = 0;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( RegionVect::Iter reg = state->regions; reg.lte(); reg++ )
			pdaTables->tokenRegions[count++] = (*reg)->id + 1;

		pdaTables->tokenRegions[count++] = 0;
	}

	/*
	 * tokenPreRegions. Build in a null at the beginning.
	 */

	count = 1;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ )
		count += state->regions.length() + 1;

	pdaTables->numPreRegionItems = count;
	pdaTables->tokenPreRegions = new int[pdaTables->numPreRegionItems];

	count = 0;
	pdaTables->tokenPreRegions[count++] = 0;
	for ( PdaStateList::Iter state = pdaGraph->stateList; state.lte(); state++ ) {
		for ( RegionVect::Iter reg = state->regions; reg.lte(); reg++ ) {
			assert( state->preRegions.length() <= 1 );
			if ( state->preRegions.length() == 0 || state->preRegions[0]->wasEmpty )
				pdaTables->tokenPreRegions[count++] = -1;
			else 
				pdaTables->tokenPreRegions[count++] = state->preRegions[0]->id + 1;
		}

		pdaTables->tokenPreRegions[count++] = 0;
	}


	return pdaTables;
}

void Compiler::makeParser( LangElSet &parserEls )
{
	pdaGraph = makePdaGraph( parserEls );
	pdaTables = makePdaTables( pdaGraph );
}

