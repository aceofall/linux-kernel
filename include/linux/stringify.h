#ifndef __LINUX_STRINGIFY_H
#define __LINUX_STRINGIFY_H

/* Indirect stringification.  Doing two levels allows the parameter to be a
 * macro itself.  For example, compile with -DFOO=bar, __stringify(FOO)
 * converts to "bar".
 */

// KID 20140204
#define __stringify_1(x...)	#x
// KID 20140204
#define __stringify(x...)	__stringify_1(x)

#endif	/* !__LINUX_STRINGIFY_H */
