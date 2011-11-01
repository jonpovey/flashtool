#ifndef __DEBUG_H__
#define __DEBUG_H__

/*
 *	Debug fprintf / printk
 */

#undef PDEBUG

#ifdef _DEBUG
#  define PDEBUG_ENABLE				// BVRDE integration
#else
#  define NDEBUG					// disable assert()
#endif

#ifndef __KERNEL__
#  include <assert.h>
#endif

//#ifndef PDEBUG_IDENTIFIER
//#	define PDEBUG_IDENTIFIER
//#endif

// Ideally there would be a better way than this of getting round
// the warning on redefinition of PDEBUG_IDENTIFIER:
#ifdef PDEBUG_IDENTIFIER
#	ifdef PDEBUG_ENABLE
#		ifdef __KERNEL__				// kernel space message
#			define PDEBUG(fmt, args...) printk(KERN_DEBUG "%s" fmt, PDEBUG_IDENTIFIER, ## args)
#		else							// userland
#			include <stdio.h>			// if not already included..
#			define PDEBUG(fmt, args...) fprintf(stderr, "%s" fmt, PDEBUG_IDENTIFIER, ##args)
#		endif
#	else
#		define PDEBUG(fmt, args...)		// do not compile
#	endif	// PDEBUG_ENABLE
#else
#	ifdef PDEBUG_ENABLE
#		ifdef __KERNEL__				// kernel space message
#			define PDEBUG(fmt, args...) printk(KERN_DEBUG fmt, ## args)
#		else							// userland
#			include <stdio.h>			// if not already included..
#			define PDEBUG(fmt, args...) fprintf(stderr, fmt, ##args)
#		endif
#	else
#		define PDEBUG(fmt, args...)		// do not compile
#	endif	// PDEBUG_ENABLE
#endif		// PDEBUG_IDENTIFIER

#define DBG(fmt, args...)  PDEBUG("%s: " fmt, __func__, ##args)

#ifdef __KERNEL__
#	define ERR(fmt, args...) printk(KERN_ERR "%s: " fmt, __func__, ##args)
#else
#	include <stdio.h>
#	define ERR(fmt, args...)  fprintf(stderr, "%s: " fmt, __func__, ##args)
#endif

#undef NOPDEBUG
#define NOPDEBUG(fmt, args...)		// NOP placeholder

#endif	// __DEBUG_H__
