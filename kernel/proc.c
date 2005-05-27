#define NEW_SCHED_Q 	1
#define OLD_SEND 	1
#define OLD_RECV 	1
/* This file contains essentially all of the process and message handling.
 * It has one main entry point from the outside:
 *
 *   sys_call: 	      a system call, i.e., the kernel is trapped with an INT 
 *
 * It also has several minor entry points to be used from the task level:
 *
 *   lock_notify:     send a notification to inform a process of a system event
 *   lock_send:	      send a message to a process
 *   lock_ready:      put a process on one of the ready queues so it can be run
 *   lock_unready:    remove a process from the ready queues
 *   lock_sched:      a process has run too long; schedule another one
 *   lock_pick_proc:  pick a process to run (used by system initialization)
 *
 * Changes:
 *   May 26, 2005     optimized message passing functions  (Jorrit N. Herder)
 *   May 24, 2005     new, queued NOTIFY system call  (Jorrit N. Herder)
 *   Oct 28, 2004     non-blocking SEND and RECEIVE  (Jorrit N. Herder)
 *   Oct 28, 2004     rewrite of sys_call()  (Jorrit N. Herder)
 *   Oct 10, 2004     require SENDREC for kernel sys_call()  (Jorrit N. Herder)
 *		      (to protect kernel tasks from being blocked)
 *   Aug 19, 2004     generalized ready()/unready()  (Jorrit N. Herder)
 */

#include "kernel.h"
#include <minix/callnr.h>
#include <minix/com.h>
#include "proc.h"
#include "ipc.h"
#include "sendmask.h"


/* Scheduling and message passing functions. The functions are available to 
 * other parts of the kernel through lock_...(). The lock temporarily disables 
 * interrupts to prevent race conditions. 
 */
FORWARD _PROTOTYPE( int mini_send, (struct proc *caller_ptr, int dst,
		message *m_ptr, unsigned flags) );
FORWARD _PROTOTYPE( int mini_rec, (struct proc *caller_ptr, int src,
		message *m_ptr, unsigned flags) );
FORWARD _PROTOTYPE( int mini_notify, (struct proc *caller_ptr, int dst,
		message *m_ptr ) );

FORWARD _PROTOTYPE( void ready, (struct proc *rp) );
FORWARD _PROTOTYPE( void sched, (int queue) );
FORWARD _PROTOTYPE( void unready, (struct proc *rp) );
FORWARD _PROTOTYPE( void pick_proc, (void) );

#define BuildMess(m,n) \
	(m).NOTIFY_SOURCE = (n)->n_source, \
	(m).NOTIFY_TYPE = (n)->n_type, \
	(m).NOTIFY_FLAGS = (n)->n_flags, \
	(m).NOTIFY_ARG = (n)->n_arg;

#if (CHIP == INTEL)
#define CopyMess(s,sp,sm,dp,dm) \
	cp_mess(s, (sp)->p_memmap[D].mem_phys, (vir_bytes)sm, (dp)->p_memmap[D].mem_phys, (vir_bytes)dm)
#endif /* (CHIP == INTEL) */

#if (CHIP == M68000)
/* M68000 does not have cp_mess() in assembly like INTEL. Declare prototype
 * for cp_mess() here and define the function below. Also define CopyMess. 
 */
#endif /* (CHIP == M68000) */


/* Declare buffer space and a bit map for notification messages. */
PRIVATE struct notification notify_buffer[NR_NOTIFY_BUFS];
PRIVATE bitchunk_t notify_bitmap[BITMAP_CHUNKS(NR_NOTIFY_BUFS)];     


/*===========================================================================*
 *				sys_call				     * 
 *===========================================================================*/
PUBLIC int sys_call(call_nr, src_dst, m_ptr)
int call_nr;			/* (NB_)SEND, (NB_)RECEIVE, SENDREC */
int src_dst;			/* src to receive from or dst to send to */
message *m_ptr;			/* pointer to message in the caller's space */
{
/* System calls are done by trapping to the kernel with an INT instruction.
 * The trap is caught and sys_call() is called to send or receive a message
 * (or both). The caller is always given by 'proc_ptr'.
 */
  register struct proc *caller_ptr = proc_ptr;	/* get pointer to caller */
  int function = call_nr & SYSCALL_FUNC;	/* get system call function */
  unsigned flags = call_nr & SYSCALL_FLAGS;	/* get flags */
  int mask_entry;				/* bit to check in send mask */
  int result;					/* the system call's result */
  vir_bytes vb;			/* message buffer pointer as vir_bytes */
  vir_clicks vlo, vhi;		/* virtual clicks containing message to send */

  /* Calls directed to the kernel may only be sendrec(), because tasks always
   * reply and may not block if the caller doesn't do receive(). Users also
   * may only use sendrec() to protect the process manager and file system.  
   */
  if (iskernel(src_dst) && function != SENDREC) 
  	return(ECALLDENIED);			/* SENDREC was required */
  
  /* Verify that requested source and/ or destination is a valid process. */
  if (! isoksrc_dst(src_dst) && function != ECHO) 
  	return(EBADSRCDST);

  /* Check validity of message pointer. */
  vb = (vir_bytes) m_ptr;
  vlo = vb >> CLICK_SHIFT;	/* vir click for bottom of message */
  vhi = (vb + MESS_SIZE - 1) >> CLICK_SHIFT;	/* vir click for top of msg */
#if ALLOW_GAP_MESSAGES
  /* This check allows a message to be anywhere in data or stack or gap. 
   * It will have to be made more elaborate later for machines which
   * don't have the gap mapped.
   */
  if (vlo < caller_ptr->p_memmap[D].mem_vir || vlo > vhi ||
      vhi >= caller_ptr->p_memmap[S].mem_vir + caller_ptr->p_memmap[S].mem_len) 
        return(EFAULT); 
#else
  /* Check for messages wrapping around top of memory or outside data seg. */
  if (vhi < vlo ||
      vhi - caller_ptr->p_memmap[D].mem_vir >= caller_ptr->p_memmap[D].mem_len) 
        return(EFAULT); 
#endif

  /* Now check if the call is known and try to perform the request. The only
   * system calls that exist in MINIX are sending and receiving messages.
   * Receiving is straightforward. Sending requires to check caller's send
   * mask and whether the destination is alive.  
   */
  switch(function) {
  case SENDREC:		
      flags |= FRESH_ANSWER;		/* ignore pending notifications */
      /* fall through */
  case SEND:			
      if (! isalive(src_dst)) { 			
          result = EDEADDST;		/* cannot send to the dead */
          break;
      }
      mask_entry = isuser(src_dst) ? USER_PROC_NR : src_dst;
      if (! isallowed(caller_ptr->p_sendmask, mask_entry)) {
          kprintf("WARNING: sys_call denied %d ", caller_ptr->p_nr);
          kprintf("sending to %d\n", proc_addr(src_dst)->p_nr);
          result = ECALLDENIED;		/* call denied by send mask */
          break;
      } 

      result = mini_send(caller_ptr, src_dst, m_ptr, flags);
      if (function == SEND || result != OK) {	
          break;				/* done, or SEND failed */
      }						/* fall through for SENDREC */
  case RECEIVE:			
      result = mini_rec(caller_ptr, src_dst, m_ptr, flags);
      break;
  case NOTIFY:
      result = mini_notify(caller_ptr, src_dst, m_ptr);
      break;
  case ECHO:
      kprintf("Echo message from process %s\n", proc_nr(caller_ptr));
      CopyMess(caller_ptr->p_nr, caller_ptr, m_ptr, caller_ptr, m_ptr);
      result = OK;
      break;
  default:
      result = EBADCALL;			/* illegal system call */
  }

  /* Now, return the result of the system call to the caller. */
  return(result);
}


/*===========================================================================*
 *				mini_send				     * 
 *===========================================================================*/
PRIVATE int mini_send(caller_ptr, dst, m_ptr, flags)
register struct proc *caller_ptr;	/* who is trying to send a message? */
int dst;				/* to whom is message being sent? */
message *m_ptr;				/* pointer to message buffer */
unsigned flags;				/* system call flags */
{
/* Send a message from 'caller_ptr' to 'dst'. If 'dst' is blocked waiting
 * for this message, copy the message to it and unblock 'dst'. If 'dst' is
 * not waiting at all, or is waiting for another source, queue 'caller_ptr'.
 */
  register struct proc *dst_ptr;
#if OLD_SEND
  register struct proc *next_ptr;
  register struct proc *xp;
#else
  register struct proc **xpp;
#endif

  dst_ptr = proc_addr(dst);	/* pointer to destination's proc entry */


  /* Check for deadlock by 'caller_ptr' and 'dst' sending to each other.
   * This check is rare, so overhead is acceptable.
   */
  if (dst_ptr->p_flags & SENDING) {
	next_ptr = proc_addr(dst_ptr->p_sendto);
	while (TRUE) {
		if (next_ptr == caller_ptr) return(ELOCKED);
		if (next_ptr->p_flags & SENDING)
			next_ptr = proc_addr(next_ptr->p_sendto);
		else
			break;
	}
  }

  /* Check if 'dst' is blocked waiting for this message. The destination's 
   * SENDING flag may be set when SENDREC couldn't send. Await a pure RECEIVE.  
   */
  if ( (dst_ptr->p_flags & (RECEIVING | SENDING)) == RECEIVING &&
       (dst_ptr->p_getfrom == ANY || dst_ptr->p_getfrom == caller_ptr->p_nr)) {
	/* Destination is indeed waiting for this message. */
	CopyMess(caller_ptr->p_nr, caller_ptr, m_ptr, dst_ptr,
		 dst_ptr->p_messbuf);
	if ((dst_ptr->p_flags &= ~RECEIVING) == 0)	
		ready(dst_ptr);
  } else if ( ! (flags & NON_BLOCKING)) {
	/* Destination is not waiting.  Block and queue caller. */
	caller_ptr->p_messbuf = m_ptr;
	if (caller_ptr->p_flags == 0) unready(caller_ptr);
	caller_ptr->p_flags |= SENDING;
	caller_ptr->p_sendto = dst;

	/* Process is now blocked.  Put in on the destination's queue. */
#if OLD_SEND
	if ( (next_ptr = dst_ptr->p_caller_q) == NIL_PROC)
		dst_ptr->p_caller_q = caller_ptr;
	else {
		while (next_ptr->p_q_link != NIL_PROC)
			next_ptr = next_ptr->p_q_link;
		next_ptr->p_q_link = caller_ptr;
	}
#else
	xpp = &dst_ptr->p_caller_q;
	while (*xpp != NIL_PROC) xpp = &(*xpp)->p_q_link;
	*xpp = caller_ptr;
#endif
	caller_ptr->p_q_link = NIL_PROC;
  } else {
        kprintf("Didn't block... ;-), flags: 0x%x\n", flags);
	return(ENOTREADY);
  }
  return(OK);
}

/*===========================================================================*
 *				mini_rec				     * 
 *===========================================================================*/
PRIVATE int mini_rec(caller_ptr, src, m_ptr, flags)
register struct proc *caller_ptr;	/* process trying to get message */
int src;				/* which message source is wanted */
message *m_ptr;				/* pointer to message buffer */
unsigned flags;				/* system call flags */
{
/* A process or task wants to get a message.  If one is already queued,
 * acquire it and deblock the sender.  If no message from the desired source
 * is available, block the caller.  
 */
#if OLD_RECV
  register struct proc *sender_ptr;
  register struct proc *previous_ptr;
#else
  register struct proc **xpp;
#endif
  register struct notification **ntf_q_pp;
  message m;
  int bit_nr;

  /* Check to see if a message from desired source is already available.
   * The caller's SENDING flag may be set if SENDREC couldn't send. If it is
   * set, the process should be blocked.
   */
  if (!(caller_ptr->p_flags & SENDING)) {

    /* Check caller queue. Use pointer pointers to keep code simple. */
#if OLD_RECV		/* to hairy, unreadable */
    for (sender_ptr = caller_ptr->p_caller_q; sender_ptr != NIL_PROC;
	 previous_ptr = sender_ptr, sender_ptr = sender_ptr->p_q_link) {
	if (src == ANY || src == proc_nr(sender_ptr)) {
		/* An acceptable message has been found. */
		CopyMess(sender_ptr->p_nr, sender_ptr,
			 sender_ptr->p_messbuf, caller_ptr, m_ptr);
		if (sender_ptr == caller_ptr->p_caller_q)
			caller_ptr->p_caller_q = sender_ptr->p_q_link;
		else
			previous_ptr->p_q_link = sender_ptr->p_q_link;
		if ((sender_ptr->p_flags &= ~SENDING) == 0)
			ready(sender_ptr);	/* deblock sender */
		return(OK);
	}
    }
#else
    xpp = &caller_ptr->p_caller_q;
    while (*xpp != NIL_PROC) {
	if (src == ANY || src == proc_nr(*xpp)) {
	    /* An acceptable source has been found. Copy the message to the 
	     * caller, update the sender's status, and remove the sender from 
	     * the caller's queue.
	     */
	    CopyMess((*xpp)->p_nr, *xpp, (*xpp)->p_messbuf, caller_ptr, m_ptr);
            if (((*xpp)->p_flags &= ~SENDING) == 0) ready(*xpp);
            *xpp = (*xpp)->p_q_link;		/* remove from queue */
            return(OK);
	}
	xpp = &(*xpp)->p_q_link;		/* proceed to next */
    }
#endif

    /* Check if there are pending notifications. */
if (! (flags & FRESH_ANSWER)) {

    ntf_q_pp = &caller_ptr->p_ntf_q;		/* get pointer pointer */
    while (*ntf_q_pp != NULL) {
	if (src == ANY || src == (*ntf_q_pp)->n_source) {
		/* Found notification. Assemble and copy message. */
		BuildMess(m, *ntf_q_pp);
                CopyMess((*ntf_q_pp)->n_source, proc_addr(HARDWARE), &m, 
                	caller_ptr, m_ptr);
                /* Remove notification from queue and bit map. */
                bit_nr = (int) (*ntf_q_pp - &notify_buffer[0]);  
                *ntf_q_pp = (*ntf_q_pp)->n_next;/* remove from queue */
                free_bit(bit_nr, notify_bitmap, NR_NOTIFY_BUFS);
                return(OK);			/* report success */
	}
	ntf_q_pp = &(*ntf_q_pp)->n_next;	/* proceed to next */
    }
}

  }

  /* No suitable message is available or the caller couldn't send in SENDREC. 
   * Block the process trying to receive, unless the flags tell otherwise.
   */
  if ( ! (flags & NON_BLOCKING)) {
      caller_ptr->p_getfrom = src;
      caller_ptr->p_messbuf = m_ptr;
      if (caller_ptr->p_flags == 0) unready(caller_ptr);
      caller_ptr->p_flags |= RECEIVING;
      return(OK);
  } else {
      return(ENOTREADY);
  }
}


/*===========================================================================*
 *				mini_notify				     * 
 *===========================================================================*/
PRIVATE int mini_notify(caller_ptr, dst, m_ptr)
register struct proc *caller_ptr;	/* process trying to notify */
int dst;				/* which process to notify */
message *m_ptr;				/* pointer to message buffer */
{
  register struct proc *dst_ptr = proc_addr(dst);
  register struct notification *ntf_p ;
  register struct notification **ntf_q_pp;
  int ntf_index;
  message ntf_mess;

  /* Check to see if target is blocked waiting for this message. A process 
   * can be both sending and receiving during a SENDREC system call.
   */
  if ( (dst_ptr->p_flags & (RECEIVING|SENDING)) == RECEIVING &&
       (dst_ptr->p_getfrom == ANY || dst_ptr->p_getfrom == caller_ptr->p_nr)) {

	/* Destination is indeed waiting for this message. */
	CopyMess(proc_nr(caller_ptr), caller_ptr, m_ptr, 
		dst_ptr, dst_ptr->p_messbuf);
	dst_ptr->p_flags &= ~RECEIVING;	/* deblock destination */
	if (dst_ptr->p_flags == 0) ready(dst_ptr);
	return(OK);
  } 

  /* Destination is not ready. Add the notification to the pending queue. 
   * Get pointer to notification message. Don't copy for the kernel. 
   */
  if (! iskernelp(caller_ptr)) {
      CopyMess(proc_nr(caller_ptr), caller_ptr, m_ptr, 
          proc_addr(HARDWARE), &ntf_mess);
      m_ptr = &ntf_mess;
  }

  /* Enqueue the message. Existing notifications with the same source
   * and type are overwritten with newer ones. New notifications that
   * are not yet on the list are added to the end.
   */
  ntf_q_pp = &dst_ptr->p_ntf_q;
  while (*ntf_q_pp != NULL) {
      /* Replace notifications with same source and type. */
      if ((*ntf_q_pp)->n_type == m_ptr->NOTIFY_TYPE && 
              (*ntf_q_pp)->n_source == proc_nr(caller_ptr)) {
          (*ntf_q_pp)->n_flags = m_ptr->NOTIFY_FLAGS;
          (*ntf_q_pp)->n_arg = m_ptr->NOTIFY_ARG;
          return(OK);
      }
      ntf_q_pp = &(*ntf_q_pp)->n_next;
  }

  /* Add to end of queue (found above). Get a free notification buffer. */
  if ((ntf_index = alloc_bit(notify_bitmap, NR_NOTIFY_BUFS)) < 0)  
      return(ENOSPC);
  ntf_p = &notify_buffer[ntf_index];
  *ntf_q_pp = ntf_p;			/* add to end of queue */
  ntf_p->n_next = NULL;			/* mark new end of queue */
  ntf_p->n_source = proc_nr(caller_ptr);
  ntf_p->n_type = m_ptr->NOTIFY_TYPE;
  ntf_p->n_flags = m_ptr->NOTIFY_FLAGS;
  ntf_p->n_arg = m_ptr->NOTIFY_ARG;
  return(OK);
}

/*==========================================================================*
 *				lock_notify				    *
 *==========================================================================*/
PUBLIC int lock_notify(dst, m_ptr)
int dst;			/* to whom is message being sent? */
message *m_ptr;			/* pointer to message buffer */
{
/* Safe gateway to mini_notify() for tasks and interrupt handlers. This 
 * function checks if it is called from an interrupt handler and makes sure
 * that the correct message source is put on the notification. 
 */
  int result;
  struct proc *caller_ptr;

  lock();
  caller_ptr = (k_reenter >= 0) ? proc_addr(HARDWARE) : proc_ptr;
  result = mini_notify(caller_ptr, dst, m_ptr); 
  unlock();
  return(result);
}

/*===========================================================================*
 *				pick_proc				     * 
 *===========================================================================*/
PRIVATE void pick_proc()
{
/* Decide who to run now.  A new process is selected by setting 'next_ptr'.
 * When a fresh user (or idle) process is selected, record it in 'bill_ptr',
 * so the clock task can tell who to bill for system time.
 */
  register struct proc *rp;			/* process to run */
  int q;					/* iterate over queues */

  /* Check each of the scheduling queues for ready processes. The number of
   * queues is defined in proc.h, and priorities are set in the task table.
   * The lowest queue contains IDLE, which is always ready.
   */
  for (q=0; q < NR_SCHED_QUEUES; q++) {	
    if ( (rp = rdy_head[q]) != NIL_PROC) {
	next_ptr = rp;				/* run process 'rp' next */
	if (isuserp(rp) || isidlep(rp)) 	/* possible bill 'rp' */
		bill_ptr = rp;
	return;
    }
  }
}

/*===========================================================================*
 *				ready					     * 
 *===========================================================================*/
PRIVATE void ready(rp)
register struct proc *rp;	/* this process is now runnable */
{
/* Add 'rp' to one of the queues of runnable processes.  */
  register int q = rp->p_priority;	/* scheduling queue to use */
  register struct proc **xpp;		/* iterate over queue */

#if ENABLE_K_DEBUGGING
  if(rp->p_ready) {
	kprintf("ready() already ready process\n", NO_NUM);
  }
  rp->p_ready = 1;
#endif

  /* Processes, in principle, are added to the end of the queue. However, 
   * user processes are added in front of the queue, because this is a bit 
   * fairer to I/O bound processes. 
   */
#if NEW_SCHED_Q
  if (isuserp(rp)) {			/* add to front of queue */
  	rp->p_nextready = rdy_head[q];	/* chain current front */
  	rdy_head[q] = rp;		/* ready process becomes front */
  }
  else {				/* add to end of queue */
  	xpp = &rdy_head[q];		/* find pointer to end of queue */
 	while (*xpp != NIL_PROC) xpp = &(*xpp)->p_nextready;
 	*xpp = rp;			/* replace end with ready process */
  	rp->p_nextready = NIL_PROC;	/* mark end of queue */
  }
#else
  if (isuserp(rp)) {	/* add to front of queue */
  	if (rdy_head[q] == NIL_PROC)
		rdy_tail[q] = rp;
  	rp->p_nextready = rdy_head[q];		/* add to front of queue */
  	rdy_head[q] = rp;
  } 
  else {
      if (rdy_head[q] != NIL_PROC)
		rdy_tail[q]->p_nextready = rp;	/* add to end of queue */
      else 
		rdy_head[q] = rp;		/* add to empty queue */
      rdy_tail[q] = rp;
      rp->p_nextready = NIL_PROC;
  }
#endif

  /* Run 'rp' next if it has a higher priority than 'proc_ptr' or 'next_ptr'. 
   * This actually should be done via pick_proc(), but the message passing 
   * functions rely on this side-effect. High priorities have a lower number.
   */
  if (next_ptr && next_ptr->p_priority > rp->p_priority) next_ptr = rp;
  else if (proc_ptr->p_priority > rp->p_priority) next_ptr = rp;
}

/*===========================================================================*
 *				unready					     * 
 *===========================================================================*/
PRIVATE void unready(rp)
register struct proc *rp;	/* this process is no longer runnable */
{
/* A process has blocked. See ready for a description of the queues. */

  register int q = rp->p_priority;	/* queue to use */
#if NEW_SCHED_Q
  register struct proc **xpp;		/* iterate over queue */
#else
  register struct proc **qtail; 	/* queue's rdy_tail */
  register struct proc *xp;
#endif

#if ENABLE_K_DEBUGGING
  if(!rp->p_ready) {
	kprintf("unready() already unready process\n", NO_NUM);
  }
  rp->p_ready = 0;
#endif

  /* Side-effect for tasks: check if the task's stack still is ok? */
  if (istaskp(rp)) { 				
	if (*rp->p_stguard != STACK_GUARD)
		panic("stack overrun by task", proc_nr(rp));
  }

  /* Now make sure that the process is not in its ready queue. Remove the 
   * process if it is found. A process can be made unready even if it is not 
   * running by being sent a signal that kills it.
   */
#if NEW_SCHED_Q
  xpp = &rdy_head[q];
  while (*xpp != NIL_PROC) {			/* check entire queue */
      if (*xpp == rp) {				/* lookup unready process */
          *xpp = (*xpp)->p_nextready;		/* replace it with next */
          if (rp == proc_ptr || rp == next_ptr)	/* current process removed */
              pick_proc();			/* pick new process to run */
          break;
      }
      xpp = &(*xpp)->p_nextready;		/* proceed to next */
  }
#else
  if ( (xp = rdy_head[q]) != NIL_PROC) {	/* ready queue is empty */
      if (xp == rp) {				/* check head of queue */
          rdy_head[q] = xp->p_nextready;	/* new head of queue */
          if (rp == proc_ptr || rp == next_ptr)	/* current process removed */
              pick_proc();			/* pick new process to run */
	if(rp == rdy_tail[q])
		rdy_tail[q] = NIL_PROC;
      }
      else {					/* check body of queue */
          while (xp->p_nextready != rp)		/* stop if process is next */
              if ( (xp = xp->p_nextready) == NIL_PROC) 
                  return;	
          xp->p_nextready = xp->p_nextready->p_nextready;
          if (rdy_tail[q] == rp) 		/* possibly update tail */
              rdy_tail[q] = xp;
      }
  }
#endif

}

/*===========================================================================*
 *				sched					     * 
 *===========================================================================*/
PRIVATE void sched(queue)
int queue;
{
/* The current process has run too long. If another low priority (user)
 * process is runnable, put the current process on the end of the user queue,
 * possibly promoting another user to head of the queue.
 */
  register struct proc **xpp;
  register struct proc *xp;

  if (rdy_head[queue] == NIL_PROC) return;

  /* One or more user processes queued. */
#if NEW_SCHED_Q

  xp = rdy_head[queue];			/* save expired process */
  rdy_head[queue] = xp->p_nextready;	/* advance to next process */ 
  xpp = &rdy_head[queue];			/* find end of queue */
  while (*xpp != NIL_PROC) xpp = &(*xpp)->p_nextready;
  *xpp = xp;					/* add expired to end */
  xp->p_nextready = NIL_PROC;			/* mark end of queue */

#else
  rdy_tail[queue]->p_nextready = rdy_head[queue];
  rdy_tail[queue] = rdy_head[queue];
  rdy_head[queue] = rdy_head[queue]->p_nextready;
  rdy_tail[queue]->p_nextready = NIL_PROC;
#endif
  pick_proc();
}

/*==========================================================================*
 *				lock_pick_proc				    *
 *==========================================================================*/
PUBLIC void lock_pick_proc()
{
/* Safe gateway to pick_proc() for tasks. */
  lock();
  pick_proc();
  unlock();
}


/*==========================================================================*
 *				lock_send				    *
 *==========================================================================*/
PUBLIC int lock_send(dst, m_ptr)
int dst;			/* to whom is message being sent? */
message *m_ptr;			/* pointer to message buffer */
{
/* Safe gateway to mini_send() for tasks. */
  int result;
  lock();
  result = mini_send(proc_ptr, dst, m_ptr, NON_BLOCKING);
  unlock();
  return(result);
}


/*==========================================================================*
 *				lock_ready				    *
 *==========================================================================*/
PUBLIC void lock_ready(rp)
struct proc *rp;		/* this process is now runnable */
{
/* Safe gateway to ready() for tasks. */
  lock();
  ready(rp);
  unlock();
}

/*==========================================================================*
 *				lock_unready				    *
 *==========================================================================*/
PUBLIC void lock_unready(rp)
struct proc *rp;		/* this process is no longer runnable */
{
/* Safe gateway to unready() for tasks. */
  lock();
  unready(rp);
  unlock();
}

/*==========================================================================*
 *				lock_sched				    *
 *==========================================================================*/
PUBLIC void lock_sched(queue)
int queue;
{
/* Safe gateway to sched() for tasks. */
  lock();
  sched(queue);
  unlock();
}

