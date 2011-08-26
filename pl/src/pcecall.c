/*  $Id$

    Part of XPCE --- The SWI-Prolog GUI toolkit

    Author:        Jan Wielemaker and Anjo Anjewierden
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi.psy.uva.nl/projects/xpce/
    Copyright (C): 1985-2002, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stdio.h>
#include <SWI-Stream.h>
#include <SWI-Prolog.h>

#ifdef __WINDOWS__

#include <windows.h>
#include <console.h>

#else /*__WINDOWS__*/

#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#define HAVE_UNISTD_H 1

#endif /*__WINDOWS__*/

#include <h/interface.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef _REENTRANT
#include <pthread.h>

static pthread_mutex_t pce_dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;
#define DLOCK() pthread_mutex_lock(&pce_dispatch_mutex)
#define DUNLOCK() pthread_mutex_unlock(&pce_dispatch_mutex)
#else
#define DLOCK()
#define DUNLOCK()
#define pthread_cleanup_push(h,a)
#define pthread_cleanup_pop(e)
#endif

#ifdef HAVE_SCHED_H
#include <sched.h>
#endif


		 /*******************************
		 *	       TYPES		*
		 *******************************/

typedef enum goal_state
{ G_WAITING,
  G_RUNNING,
  G_TRUE,
  G_FALSE,
  G_ERROR
} goal_state;

typedef struct
{ module_t	module;			/* module to call in */
  record_t	goal;			/* the term to call */
  int		acknowledge;		/* If set, wait ( */
  goal_state	state;			/* G_* */
#ifdef __WINDOWS__
  DWORD		client;			/* id of client thread */
#endif
} prolog_goal;


typedef struct
{ int			pce_thread;
  PL_dispatch_hook_t	input_hook;
  int			input_hook_saved;
#ifdef __WINDOWS__
  HINSTANCE		hinstance;
  HWND			window;
  RlcUpdateHook		update_hook;
#else /*__WINDOWS__*/
  int			pipe[2];
  XtInputId		id;
#endif /*__WINDOWS__*/
} context_t;

static int init_prolog_goal(prolog_goal *g, term_t goal, int acknowledge);
static void call_prolog_goal(prolog_goal *g);

static context_t context;


		 /*******************************
		 *	       ERRORS		*
		 *******************************/

static int
resource_error(const char *error)
{ term_t ex;

  if ( (ex = PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR_CHARS, "error", 2,
		       PL_FUNCTOR_CHARS, "resource_error", 1,
		         PL_CHARS, error,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}

static int
type_error(term_t actual, const char *expected)
{ term_t ex = PL_new_term_ref();

  if ( (ex = PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR_CHARS, "error", 2,
		       PL_FUNCTOR_CHARS, "type_error", 2,
		         PL_CHARS, expected,
		         PL_TERM, actual,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}

#ifdef __WINDOWS__

		 /*******************************
		 *	  WINDOWS SOLUTION	*
		 *******************************/

#define WM_CALL		(WM_USER+56)
#define WM_CALL_DONE	(WM_USER+57)

static LRESULT WINAPI
call_wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{ switch( message )
  { case WM_CALL:
    { prolog_goal *g = (prolog_goal *)lParam;

      call_prolog_goal(g);
      if ( g->acknowledge )
      { PostThreadMessage(g->client, WM_CALL_DONE, 0, 0);
      } else
      { PL_free(g);
      }
      pceRedraw(FALSE);

      return 0;
    }
  }

  return DefWindowProc(hwnd, message, wParam, lParam);
}

static char *
HiddenFrameClass()
{ static char *name;
  static WNDCLASS wndClass;

  if ( !name )
  { char buf[50];

    context.hinstance = GetModuleHandle("xpce2pl");
    sprintf(buf, "PceCallWin%d", (int)context.hinstance);
    name = strdup(buf);

    wndClass.style		= 0;
    wndClass.lpfnWndProc	= (LPVOID) call_wnd_proc;
    wndClass.cbClsExtra		= 0;
    wndClass.cbWndExtra		= 0;
    wndClass.hInstance		= context.hinstance;
    wndClass.hIcon		= NULL;
    wndClass.hCursor		= NULL;
    wndClass.hbrBackground	= GetStockObject(WHITE_BRUSH);
    wndClass.lpszMenuName	= NULL;
    wndClass.lpszClassName	= name;

    RegisterClass(&wndClass);
  }

  return name;
}


static void
unsetup(int code, void *closure)
{ if ( context.window )
  { DestroyWindow(context.window);
    context.window = 0;
  }
}


static int
setup()
{ if ( context.window )
    return TRUE;

  DLOCK();
  if ( !context.window )
  { context.window = CreateWindow(HiddenFrameClass(),
				  "XPCE/SWI-Prolog call window",
				  WS_POPUP,
				  0, 0, 32, 32,
				  NULL, NULL, context.hinstance, NULL);
    PL_on_halt(unsetup, NULL);
  }
  DUNLOCK();

  return TRUE;
}


static foreign_t
in_pce_thread(term_t goal)
{ prolog_goal *g = PL_malloc(sizeof(*g));

  if ( !init_prolog_goal(g, goal, FALSE) )
  { PL_free(g);
    return FALSE;
  }

  PostMessage(context.window, WM_CALL, (WPARAM)0, (LPARAM)g);

  return TRUE;
}


static foreign_t
in_pce_thread_sync(term_t goal)
{ prolog_goal *g = PL_malloc(sizeof(*g));
  MSG msg;
  int rc = FALSE;

  if ( !init_prolog_goal(g, goal, TRUE) )
  { PL_free(g);
    return FALSE;
  }

  g->client = GetCurrentThreadId();
  PostMessage(context.window, WM_CALL, (WPARAM)0, (LPARAM)g);

  while( GetMessage(&msg, NULL, 0, 0) )
  { TranslateMessage(&msg);
    DispatchMessage(&msg);
    if ( PL_handle_signals() < 0 )
      return FALSE;

    switch(g->state)
    { case G_TRUE:
	rc = TRUE;
        goto out;
      case G_FALSE:
	goto out;
      case G_ERROR:
      { term_t ex = PL_new_term_ref();

	if ( PL_recorded(g->goal, ex) )
	  rc = PL_raise_exception(ex);
	goto out;
      }
      default:
	continue;
    }
  }

out:
  PL_free(g);
  return rc;
}


#else /*!__WINDOWS__*/

		 /*******************************
		 *	   X11 SCHEDULING	*
		 *******************************/

static void
on_input(XtPointer xp, int *source, XtInputId *id)
{ context_t *ctx = (context_t *)xp;
  prolog_goal g;
  int n;

  if ( (n=read(ctx->pipe[0], &g, sizeof(g))) == sizeof(g) )
  { call_prolog_goal(&g);
  } else if ( n == 0 )		/* EOF: quit */
  { close(ctx->pipe[0]);
    ctx->pipe[0] = -1;
  }
}


static int
setup()
{ if ( context.pipe[0] > 0 )
    return TRUE;

  DLOCK();
  if ( context.pipe[0] == -1 )
  { if ( pipe(context.pipe) == -1 )
    { DUNLOCK();
      return resource_error("open_files");
    }

    context.id = XtAppAddInput(pceXtAppContext(NULL),
			       context.pipe[0],
			       (XtPointer)(XtInputReadMask),
			       on_input, &context);
  }
  DUNLOCK();

  return TRUE;
}


static foreign_t
in_pce_thread(term_t goal)
{ prolog_goal g;
  int rc;

  if ( !setup() )
    return FALSE;

  if ( !init_prolog_goal(&g, goal, FALSE) )
    return FALSE;

  rc = write(context.pipe[1], &g, sizeof(g));

  if ( rc == sizeof(g) )
    return TRUE;

  return FALSE;
}


static foreign_t
in_pce_thread_sync(term_t goal)
{ prolog_goal g;
  int rc;

  if ( !setup() )
    return FALSE;

  if ( !init_prolog_goal(&g, goal, FALSE) )
    return FALSE;

  rc = write(context.pipe[1], &g, sizeof(g));

  if ( rc == sizeof(g) )
    return TRUE;

  return FALSE;
}

#endif /*!__WINDOWS__*/


		 /*******************************
		 *	CREATE/EXECUTE GOAL	*
		 *******************************/

static int
init_prolog_goal(prolog_goal *g, term_t goal, int acknowledge)
{ term_t plain = PL_new_term_ref();

  g->module	 = NULL;
  g->acknowledge = acknowledge;
  g->state       = G_WAITING;
  PL_strip_module(goal, &g->module, plain);
  if ( !(PL_is_compound(plain) || PL_is_atom(plain)) )
    return type_error(goal, "callable");
  g->goal = PL_record(plain);

  return TRUE;
}


static void
call_prolog_goal(prolog_goal *g)
{ fid_t fid;
  static predicate_t pred = NULL;
  int rc;

  if ( !pred )
    pred = PL_predicate("call", 1, "user");

  if ( (fid = PL_open_foreign_frame()) )
  { term_t t = PL_new_term_ref();
    rc = PL_recorded(g->goal, t);
    PL_erase(g->goal);
    g->goal = 0;
    g->state = G_RUNNING;
    if ( rc )
    { qid_t qid;
      int flags = PL_Q_NORMAL;

      if ( g->acknowledge )
	flags |= PL_Q_CATCH_EXCEPTION;


      if ( (qid = PL_open_query(g->module, flags, pred, t)) )
      { rc = PL_next_solution(qid);

	if ( rc )
	{ g->state = G_TRUE;
	} else
	{ term_t ex;

	  if ( g->acknowledge && (ex=PL_exception(qid)) )
	  { g->goal = PL_record(ex);
	    g->state = G_ERROR;
	  } else
	  { g->state = G_FALSE;
	  }
	}

	PL_cut_query(qid);
      } else
	PL_warning("ERROR: pce: out of global stack");
    }
    PL_discard_foreign_frame(fid);
  } else
    PL_warning("ERROR: pce: out of global stack");
}


#ifdef __WINDOWS__
/* from interface.c */
extern RlcUpdateHook indirect_rlc_update_hook(RlcUpdateHook hook);
#endif

static foreign_t
set_pce_thread()
{ int tid = PL_thread_self();

  if ( tid != context.pce_thread )
  { context.pce_thread = tid;

    if ( context.input_hook_saved )
    { PL_dispatch_hook(context.input_hook);
#ifdef __WINDOWS__
      indirect_rlc_update_hook(context.update_hook);
#endif
      context.input_hook_saved = FALSE;
    }

#ifdef __WINDOWS__
    if ( context.window )
    { DestroyWindow(context.window);
      context.window = 0;
    }
    setPceThread(GetCurrentThreadId());
    setup();
#endif

    if ( context.pce_thread != 1 )
    { context.input_hook = PL_dispatch_hook(NULL);
#ifdef __WINDOWS__
      context.update_hook = indirect_rlc_update_hook(NULL);
#endif
      context.input_hook_saved = TRUE;
    }
  }

  return TRUE;
}


static foreign_t
pl_pce_dispatch()
{ pceDispatch(-1, 250);

  if ( PL_handle_signals() == -1 || PL_exception(0) )
    return FALSE;

  return TRUE;
}



		 /*******************************
		 *	       INSTALL		*
		 *******************************/

install_t
install_pcecall()
{ context.pce_thread = PL_thread_self();

#ifdef __WINDOWS__
  setup();
#else
  context.pipe[0] = context.pipe[1] = -1;
#endif

  PL_register_foreign("in_pce_thread",      1,
		      in_pce_thread, PL_FA_META, "0");
  PL_register_foreign("in_pce_thread_sync", 1,
		      in_pce_thread_sync, PL_FA_META, "0");
  PL_register_foreign("set_pce_thread", 0, set_pce_thread, 0);
  PL_register_foreign("pce_dispatch",   0, pl_pce_dispatch, 0);
}
