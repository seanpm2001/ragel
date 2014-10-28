/*
 *  Copyright 2002 Adrian Thurston <thurston@complang.org>
 */

#ifndef _AAPL_AVLMEL_H
#define _AAPL_AVLMEL_H

#include "compare.h"

/**
 * \addtogroup avltree 
 * @{
 */

/**
 * \class AvlMel
 * \brief AVL tree for elements appearing in multiple trees.
 *
 * AvlMel allows for an element to simultaneously be in multiple trees without
 * the trees interferring with one another. For each tree that the element is
 * to appear in, there must be a distinct set of AVL Tree management data that
 * can be unambiguously referenced with some base class name. This name
 * is passed to the tree as a template parameter and is used in the tree
 * algorithms.
 *
 * The element must use the same key type and value in each tree that it
 * appears in. If distinct keys are required, the AvlMelKey structure is
 * available.
 *
 * AvlMel does not assume ownership of elements in the tree. The destructor
 * will not delete the elements. If the user wishes to explicitly deallocate
 * all the items in the tree the empty() routine is available. 
 *
 * \include ex_avlmel.cpp
 */

/*@}*/

#define BASE_EL(name) BaseEl::name
#define BASEKEY(name) name
#define AVLMEL_CLASSDEF class Element, class Key, \
		class BaseEl, class Compare = CmpOrd<Key>
#define AVLMEL_TEMPDEF class Element, class Key, \
		class BaseEl, class Compare
#define AVLMEL_TEMPUSE Element, Key, BaseEl, Compare
#define AvlTree AvlMel

#include "avlcommon.h"

#undef BASE_EL
#undef BASEKEY
#undef AVLMEL_CLASSDEF
#undef AVLMEL_TEMPDEF
#undef AVLMEL_TEMPUSE
#undef AvlTree

#endif /* _AAPL_AVLMEL_H */
