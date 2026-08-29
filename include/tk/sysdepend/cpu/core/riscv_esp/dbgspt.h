#ifndef __TK_DBGSPT_DEPEND_CORE_H__
#define __TK_DBGSPT_DEPEND_CORE_H__

typedef struct td_calinf {
	void	*ssp;
	void	*fp;		/* RISC-V frame pointer (s0/x8) */
} TD_CALINF;

#endif
