//    Copyright (C) Mike Rieker, Beverly, MA USA
//    www.outerworldapps.com
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    EXPECT it to FAIL when someone's HeALTh or PROpeRTy is at RISk.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//    http://www.gnu.org/licenses/gpl-2.0.html

// run via pipan8l shell script:
//  ./pipan8l [-sim] [<scriptfile.tcl>]

#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <tcl.h>
#include <unistd.h>

#include "assemble.h"
#include "disassemble.h"
#include "padlib.h"
#include "pindefs.h"
#include "readprompt.h"
#include "udppkt.h"

#define ABORT() do { fprintf (stderr, "abort() %s:%d\n", __FILE__, __LINE__); abort (); } while (0)
#define ASSERT(cond) do { if (__builtin_constant_p (cond)) { if (!(cond)) asm volatile ("assert failure line %c0" :: "i"(__LINE__)); } else { if (!(cond)) ABORT (); } } while (0)

struct FunDef {
    Tcl_ObjCmdProc *func;
    char const *name;
    char const *help;
};

struct PermSw {
    char const *name;
    int nbits;
    int pinums[12];
};

struct RegBit {
    char const *name;
    int pinum;
};

// internal TCL commands
static Tcl_ObjCmdProc cmd_assemop;
static Tcl_ObjCmdProc cmd_ctrlcflag;
static Tcl_ObjCmdProc cmd_disasop;
static Tcl_ObjCmdProc cmd_flushit;
static Tcl_ObjCmdProc cmd_getpin;
static Tcl_ObjCmdProc cmd_getreg;
static Tcl_ObjCmdProc cmd_getsw;
static Tcl_ObjCmdProc cmd_help;
static Tcl_ObjCmdProc cmd_setpin;
static Tcl_ObjCmdProc cmd_setsw;

static FunDef const fundefs[] = {
    { cmd_assemop,    "assemop",    "assemble instruction" },
    { cmd_ctrlcflag,  "ctrlcflag",  "read and clear control-C flag" },
    { cmd_disasop,    "disasop",    "disassemble instruction" },
    { cmd_flushit,    "flushit",    "flush writes / invalidate reads" },
    { cmd_getpin,     "getpin",     "get gpio pin" },
    { cmd_getreg,     "getreg",     "get register value" },
    { cmd_getsw,      "getsw",      "get switch value" },
    { cmd_help,       "help",       "print this help" },
    { cmd_setpin,     "setpin",     "set gpio pin" },
    { cmd_setsw,      "setsw",      "set switch value" },
    { NULL, NULL, NULL }
};

static int const acbits[] = { P_AC00, P_AC01, P_AC02, P_AC03, P_AC04, P_AC05, P_AC06, P_AC07, P_AC08, P_AC09, P_AC10, P_AC11, -1 };
static int const irbits[] = { P_IR00, P_IR01, P_IR02, -1 };
static int const mabits[] = { P_MA00, P_MA01, P_MA02, P_MA03, P_MA04, P_MA05, P_MA06, P_MA07, P_MA08, P_MA09, P_MA10, P_MA11, -1 };
static int const mbbits[] = { P_MB00, P_MB01, P_MB02, P_MB03, P_MB04, P_MB05, P_MB06, P_MB07, P_MB08, P_MB09, P_MB10, P_MB11, -1 };
static int const srbits[] = { P_SR00, P_SR01, P_SR02, P_SR03, P_SR04, P_SR05, P_SR06, P_SR07, P_SR08, P_SR09, P_SR10, P_SR11, -1 };

static PermSw const permsws[] = {
    { "bncy",  1, P_BNCY },
    { "cont",  1, P_CONT },
    { "dep",   1, P_DEP  },
    { "dfld",  1, P_DFLD },
    { "exam",  1, P_EXAM },
    { "ifld",  1, P_IFLD },
    { "ldad",  1, P_LDAD },
    { "mprt",  1, P_MPRT },
    { "sr", 12, P_SR11, P_SR10, P_SR09, P_SR08, P_SR07, P_SR06, P_SR05, P_SR04, P_SR03, P_SR02, P_SR01, P_SR00 },
    { "start", 1, P_STRT },
    { "step",  1, P_STEP },
    { "stop",  1, P_STOP },
    {  NULL,   0, 0 }
};

static uint16_t const wrmsks[P_NU16S] = { P0_WMSK, P1_WMSK, P2_WMSK, P3_WMSK, P4_WMSK };

static bool volatile ctrlcflag;
static bool rdpadsvalid;
static bool wrpadsdirty;
static char *inihelp;
static int logfd, pipefds[2], oldstdoutfd;
static PadLib *padlib;
static pthread_mutex_t padmutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t logtid;
static sigset_t sigintmask;
static uint16_t rdpads[P_NU16S];
static uint16_t wrpads[P_NU16S];

static void siginthand (int signum);
static void *logthread (void *dummy);
static void closelog ();
static int writepipe (int fd, char const *buf, int len);
static void Tcl_SetResultF (Tcl_Interp *interp, char const *fmt, ...);
static uint16_t getreg (int const *pins);
static void setpin (int pin, bool set);
static bool getpin (int pin);
static void flushit ();
static void *udpthread (void *dummy);



int main (int argc, char **argv)
{
    setlinebuf (stdout);
    sigemptyset (&sigintmask);
    sigaddset (&sigintmask, SIGINT);

    bool simit = false;
    char const *fn = NULL;
    char const *logname = NULL;
    for (int i = 0; ++ i < argc;) {
        if (strcasecmp (argv[i], "-log") == 0) {
            if ((++ i >= argc) || (argv[i][0] == '-')) {
                fprintf (stderr, "missing filename after -log\n");
                return 1;
            }
            logname = argv[i];
            continue;
        }
        if (strcasecmp (argv[i], "-sim") == 0) {
            simit = true;
            continue;
        }
        if ((argv[i][0] == '-') || (fn != NULL)) {
            fprintf (stderr, "usage: %s [-log <logfilename>] [-simit] [<tclfilename>]\n", argv[0]);
            return 1;
        }
        fn = argv[i];
    }

    padlib = simit ? (PadLib *) new SimLib () : (PadLib *) new I2CLib ();
    padlib->openpads ();
    // initialize switches from existing switch states
    padlib->readpads (rdpads);
    for (int i = 0; i < P_NU16S; i ++) wrpads[i] = rdpads[i];

    Tcl_FindExecutable (argv[0]);

    Tcl_Interp *interp = Tcl_CreateInterp ();
    if (interp == NULL) ABORT ();
    int rc = Tcl_Init (interp);
    if (rc != TCL_OK) {
        char const *err = Tcl_GetStringResult (interp);
        if ((err != NULL) && (err[0] != 0)) {
            fprintf (stderr, "pipan8l: error %d initialing tcl: %s\n", rc, err);
        } else {
            fprintf (stderr, "pipan8l: error %d initialing tcl\n", rc);
        }
        ABORT ();
    }

    // https://www.tcl-lang.org/man/tcl/TclLib/CrtObjCmd.htm

    for (int i = 0; fundefs[i].name != NULL; i ++) {
        if (Tcl_CreateObjCommand (interp, fundefs[i].name, fundefs[i].func, NULL, NULL) == NULL) ABORT ();
    }

    // redirect stdout if -log given
    if (logname != NULL) {
        fflush (stdout);
        fflush (stderr);

        // open log file
        logfd = open (logname, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (logfd < 0) {
            fprintf (stderr, "error creating %s: %m\n", logname);
            return 1;
        }

        // save old stdout so we can fclose stdout then fclose it
        oldstdoutfd = dup (STDOUT_FILENO);
        if (oldstdoutfd < 0) ABORT ();

        // create pipe that will be used for new stdout then open stdout on it
        if (pipe (pipefds) < 0) ABORT ();
        fclose (stdout);  // fclose() after pipe() so pipefds[0] doesn't get STDOUT_FILENO
        if (dup2 (pipefds[1], STDOUT_FILENO) < 0) ABORT ();
        close (pipefds[1]);
        pipefds[1] = -1;
        stdout = fdopen (STDOUT_FILENO, "w");
        if (stdout == NULL) ABORT ();
        setlinebuf (stdout);

        // logfd = log file
        // stdout = write end of pipe
        // oldstdout = original stdout (probably a tty)
        // pipefds[0] = read end of pipe

        // create thread to copy from pipe to both old stdout and log file
        if (pthread_create (&logtid, NULL, logthread, NULL) < 0) ABORT ();
        pthread_detach (logtid);
        atexit (closelog);

        // \377 means only write to log file, not to old stdout
        printf ("\377*S*T*A*R*T*U*P*\n");
    }

    // create udp server thread
    {
        pthread_t udptid;
        int rc = pthread_create (&udptid, NULL, udpthread, NULL);
        if (rc != 0) ABORT ();
    }

    // set ctrlcflag on control-C, but exit if two in a row
    signal (SIGINT, siginthand);

    // maybe there is a script init file
    char const *scriptini = getenv ("pipan8lini");
    if (scriptini != NULL) {
        rc = Tcl_EvalFile (interp, scriptini);
        if (rc != TCL_OK) {
            char const *err = Tcl_GetStringResult (interp);
            if ((err == NULL) || (err[0] == 0)) fprintf (stderr, "pipan8l: error %d evaluating scriptini %s\n", rc, scriptini);
                                  else fprintf (stderr, "pipan8l: error %d evaluating scriptini %s: %s\n", rc, scriptini, err);
            Tcl_EvalEx (interp, "puts $::errorInfo", -1, TCL_EVAL_GLOBAL);
            return 1;
        }
        char const *res = Tcl_GetStringResult (interp);
        if ((res != NULL) && (res[0] != 0)) inihelp = strdup (res);
    }

    // if given a filename, process that file as a whole
    if (fn != NULL) {
        rc = Tcl_EvalFile (interp, fn);
        if (rc != TCL_OK) {
            char const *res = Tcl_GetStringResult (interp);
            if ((res == NULL) || (res[0] == 0)) fprintf (stderr, "pipan8l: error %d evaluating script %s\n", rc, fn);
                                  else fprintf (stderr, "pipan8l: error %d evaluating script %s: %s\n", rc, fn, res);
            Tcl_EvalEx (interp, "puts $::errorInfo", -1, TCL_EVAL_GLOBAL);
            return 1;
        }
    }

    // either way, prompt and process commands from stdin
    // to have a script file with no stdin processing, end script file with 'exit'
    puts ("\nTCL scripting, do 'help' for pipan8l-specific commands");
    puts ("  do 'exit' to exit pipan8l");
    for (char const *line;;) {
        ctrlcflag = false;
        line = readprompt ("pipan8l> ");
        ctrlcflag = false;
        if (line == NULL) break;
        rc = Tcl_EvalEx (interp, line, -1, TCL_EVAL_GLOBAL);
        char const *res = Tcl_GetStringResult (interp);
        if (rc != TCL_OK) {
            if ((res == NULL) || (res[0] == 0)) fprintf (stderr, "pipan8l: error %d evaluating command\n", rc);
                                  else fprintf (stderr, "pipan8l: error %d evaluating command: %s\n", rc, res);
            Tcl_EvalEx (interp, "puts $::errorInfo", -1, TCL_EVAL_GLOBAL);
        }
        else if ((res != NULL) && (res[0] != 0)) puts (res);
    }

    Tcl_Finalize ();

    return 0;
}

static void siginthand (int signum)
{
    if ((isatty (STDIN_FILENO) > 0) && (write (STDIN_FILENO, "\n", 1) > 0)) { }
    if (logfd > 0) writepipe (STDOUT_FILENO, "\377^C\n", 4);
    if (ctrlcflag) exit (1);
    ctrlcflag = true;
}

// read from pipefds[0] and write to both logfd and oldstdoutfd
// need this instead of 'tee' command cuz control-C aborts 'tee'
static void *logthread (void *dummy)
{
    bool atbol = true;
    bool echoit = true;
    char buff[4096];
    int rc;
    while ((rc = read (pipefds[0], buff, sizeof buff)) > 0) {
        struct timeval nowtv;
        if (gettimeofday (&nowtv, NULL) < 0) ABORT ();
        struct tm nowtm = *localtime (&nowtv.tv_sec);

        int ofs = 0;
        int len;
        while ((len = rc - ofs) > 0) {

            // \377 means write following line only to log file, don't echo to old stdout
            char *p = (char *) memchr (buff + ofs, '\377', len);
            if (p != NULL) len = p - buff - ofs;
            if (len == 0) {
                echoit = false;
                ofs ++;
                continue;
            }

            // chop amount being written to one line so we can prefix next line with timestamp
            char *q = (char *) memchr (buff + ofs, '\n', len);
            if (q != NULL) len = ++ q - buff - ofs;

            // maybe echo to old stdout
            if (echoit && (writepipe (oldstdoutfd, buff + ofs, len) < 0)) {
                fprintf (stderr, "logthread: error writing to stdout: %m\n");
                ABORT ();
            }

            // always wtite to log file with timestamp
            if (atbol) dprintf (logfd, "%02d:%02d:%02d.%06d ", nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec, (int) nowtv.tv_usec);
            if (writepipe (logfd, buff + ofs, len) < 0) {
                fprintf (stderr, "logthread: error writing to logfile: %m\n");
                ABORT ();
            }

            // if just reached eol, re-enable stdout echoing and remember to timestamp next line
            ofs    += len;
            atbol   = (buff[ofs-1] == '\n');
            echoit |= atbol;
        }
    }
    close (logfd);
    return NULL;
}

// make sure log file gets last bit written to stdout
static void closelog ()
{
    fclose (stdout);
    stdout = NULL;
    pthread_join (logtid, NULL);
    logtid = 0;
}

static int writepipe (int fd, char const *buf, int len)
{
    while (len > 0) {
        int rc = write (fd, buf, len);
        if (len <= 0) {
            if (len == 0) errno = EPIPE;
            return -1;
        }
        buf += rc;
        len -= rc;
    }
    return 0;
}



// assemble instruciton
//  assemop [address] opcode
static int cmd_assemop (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    if (objc > 1) {
        int i = 1;
        char const *stri = Tcl_GetString (objv[i]);
        if (strcasecmp (stri, "help") == 0) {
            puts ("");
            puts ("  assemop [<address>] <opcode>...");
            puts ("   address = integer address of the opcode (for page-relative addresses)");
            puts ("    opcode = opcode string(s)");
            puts ("");
            return TCL_OK;
        }

        char *p;
        uint16_t address = strtoul (stri, &p, 0);
        if ((objc == 2) || (p == stri) || (*p != 0)) address = 0200;
                              else stri = Tcl_GetString (objv[++i]);

        std::string opstr;
        while (true) {
            opstr.append (stri);
            if (++ i >= objc) break;
            opstr.push_back (' ');
            stri = Tcl_GetString (objv[i]);
        }

        uint16_t opcode;
        char *err = assemble (opstr.c_str (), address, &opcode);
        if (err != NULL) {
            Tcl_SetResult (interp, err, (void (*) (char *)) free);
            return TCL_ERROR;
        }
        Tcl_SetObjResult (interp, Tcl_NewIntObj (opcode));
        return TCL_OK;
    }

    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// read and clear the control-C flag
static int cmd_ctrlcflag (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    bool oldctrlcflag = ctrlcflag;
    switch (objc) {
        case 2: {
            char const *opstr = Tcl_GetString (objv[1]);
            if (strcasecmp (opstr, "help") == 0) {
                puts ("");
                puts ("  ctrlcflag - read control-C flag");
                puts ("  ctrlcflag <boolean> - read control-C flag and set to given value");
                puts ("");
                puts ("  in any case, control-C flag is cleared at command prompt");
                puts ("");
                return TCL_OK;
            }
            int temp;
            int rc = Tcl_GetBooleanFromObj (interp, objv[1], &temp);
            if (rc != TCL_OK) return rc;
            if (sigprocmask (SIG_BLOCK, &sigintmask, NULL) != 0) ABORT ();
            oldctrlcflag = ctrlcflag;
            ctrlcflag = temp != 0;
            if (sigprocmask (SIG_UNBLOCK, &sigintmask, NULL) != 0) ABORT ();
            // fallthrough
        }
        case 1: {
            Tcl_SetObjResult (interp, Tcl_NewIntObj (oldctrlcflag));
            return TCL_OK;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// disassemble instruciton
//  disasop opcode [address]
static int cmd_disasop (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    int opcode = -1;
    int addr = 0200;

    switch (objc) {
        case 2: {
            char const *opstr = Tcl_GetString (objv[1]);
            if (strcasecmp (opstr, "help") == 0) {
                puts ("");
                puts ("  disasop <opcode> [<address>]");
                puts ("    opcode = integer opcode");
                puts ("   address = integer address of the opcode (for page-relative addresses)");
                puts ("");
                return TCL_OK;
            }
            int rc = Tcl_GetIntFromObj (interp, objv[1], &opcode);
            if (rc != TCL_OK) return rc;
            break;
        }
        case 3: {
            int rc = Tcl_GetIntFromObj (interp, objv[1], &opcode);
            if (rc == TCL_OK) rc = Tcl_GetIntFromObj (interp, objv[2], &addr);
            if (rc != TCL_OK) return rc;
            break;
        }
        default: {
            Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
            return TCL_ERROR;
        }
    }

    std::string str = disassemble (opcode, addr);
    Tcl_SetResult (interp, strdup (str.c_str ()), (void (*) (char *)) free);
    return TCL_OK;
}

static int cmd_flushit (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    switch (objc) {
        case 1: {
            if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
            flushit ();
            rdpadsvalid = false;
            if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();
            return TCL_OK;
        }
        case 2: {
            char const *swname = Tcl_GetString (objv[1]);
            if (strcasecmp (swname, "help") == 0) {
                puts ("");
                puts (" flushit");
                puts ("   flush pending writes to paddles");
                puts ("   invalidate cached paddle reads");
                puts ("");
                return TCL_OK;
            }
            break;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// get gpio pin
static int cmd_getpin (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    switch (objc) {
        case 2: {
            char const *regname = Tcl_GetString (objv[1]);
            if (strcasecmp (regname, "help") == 0) {
                puts ("");
                puts (" getpin <pinnumber>");
                puts ("   0.. 7 : U1 GPA0..7");
                puts ("   8..15 : U1 GPB0..7");
                puts ("  16..23 : U2 GPA0..7");
                puts ("  24..31 : U2 GPB0..7");
                puts ("  32..39 : U3 GPA0..7");
                puts ("  40..47 : U3 GPB0..7");
                puts ("  48..55 : U4 GPA0..7");
                puts ("  56..63 : U4 GPB0..7");
                puts ("  64..71 : U5 GPA0..7");
                puts ("  72..79 : U5 GPB0..7");
                puts ("");
                return TCL_OK;
            }
            int pinnum;
            int rc = Tcl_GetIntFromObj (interp, objv[1], &pinnum);
            if (rc != TCL_OK) return rc;
            if ((pinnum < 0) || (pinnum > 79)) {
                Tcl_SetResultF (interp, "bad pin number %d\n", pinnum);
                return TCL_ERROR;
            }
            int pinval = padlib->readpin (pinnum);
            if (pinval < 0) {
                Tcl_SetResultF (interp, "error reading pin %d\n", pinnum);
                return TCL_ERROR;
            }
            Tcl_SetObjResult (interp, Tcl_NewIntObj (pinval));
            return TCL_OK;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// get register (lights)
static int cmd_getreg (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    switch (objc) {
        case 2: {
            char const *regname = Tcl_GetString (objv[1]);
            if (strcasecmp (regname, "help") == 0) {
                puts ("");
                puts (" getreg <registername>");
                puts ("   multi-bit registers:");
                puts ("      ac - accumulator");
                puts ("      ir - instruction register 0..7");
                puts ("      ma - memory address");
                puts ("      mb - memory buffer");
                puts ("   state - which of F E D WC CA B are lit");
                puts ("   single-bit registers:");
                puts ("      ea - extended address");
                puts ("    link - link bit");
                puts ("     ion - interrupts enabled");
                puts ("     par - memory parity error");
                puts ("    prot - memory protect error");
                puts ("     run - executing instructions");
                puts ("");
                return TCL_OK;
            }
            int const *regpins = NULL;
            if (strcasecmp (regname, "ac") == 0) regpins = acbits;
            if (strcasecmp (regname, "ir") == 0) regpins = irbits;
            if (strcasecmp (regname, "ma") == 0) regpins = mabits;
            if (strcasecmp (regname, "mb") == 0) regpins = mbbits;
            if (regpins != NULL) {
                if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
                int regval = getreg (regpins);
                if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();
                Tcl_SetObjResult (interp, Tcl_NewIntObj (regval));
                return TCL_OK;
            }
            if (strcasecmp (regname, "state") == 0) {
                char *statebuf = (char *) malloc (16);
                if (statebuf == NULL) ABORT ();
                char *p = statebuf;
                if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
                if (getpin (P_FET)) { strcpy (p, "F ");  p += strlen (p); }
                if (getpin (P_EXE)) { strcpy (p, "E ");  p += strlen (p); }
                if (getpin (P_DEF)) { strcpy (p, "D ");  p += strlen (p); }
                if (getpin (P_WCT)) { strcpy (p, "WC "); p += strlen (p); }
                if (getpin (P_CAD)) { strcpy (p, "CA "); p += strlen (p); }
                if (getpin (P_BRK)) { strcpy (p, "B ");  p += strlen (p); }
                if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();
                if (p > statebuf) -- p;
                *p = 0;
                Tcl_SetResult (interp, statebuf, (void (*) (char *)) free);
                return TCL_OK;
            }
            int pinum = -1;
            if (strcasecmp (regname, "ea")   == 0) pinum = P_EMA;
            if (strcasecmp (regname, "ion")  == 0) pinum = P_ION;
            if (strcasecmp (regname, "link") == 0) pinum = P_LINK;
            if (strcasecmp (regname, "par")  == 0) pinum = P_PARE;
            if (strcasecmp (regname, "prot") == 0) pinum = P_PRTE;
            if (strcasecmp (regname, "run")  == 0) pinum = P_RUN;
            if (pinum >= 0) {
                if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
                int regval = getpin (pinum);
                if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();
                Tcl_SetObjResult (interp, Tcl_NewIntObj (regval));
                return TCL_OK;
            }
            Tcl_SetResultF (interp, "bad register name %s", regname);
            return TCL_ERROR;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// get switch
static int cmd_getsw (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    switch (objc) {
        case 2: {
            char const *swname = Tcl_GetString (objv[1]);
            if (strcasecmp (swname, "help") == 0) {
                puts ("");
                puts (" getsw <switchname>");
                for (int i = 0; permsws[i].name != NULL; i ++) {
                    printf ("      %s\n", permsws[i].name);
                }
                puts ("");
                return TCL_OK;
            }
            for (int i = 0; permsws[i].name != NULL; i ++) {
                if (strcasecmp (swname, permsws[i].name) == 0) {
                    int swval = 0;
                    if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
                    for (int j = permsws[i].nbits; -- j >= 0;) {
                        swval <<= 1;
                        swval += getpin (permsws[i].pinums[j]);
                    }
                    if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();
                    Tcl_SetObjResult (interp, Tcl_NewIntObj (swval));
                    return TCL_OK;
                }
            }
            Tcl_SetResultF (interp, "bad switch name %s", swname);
            return TCL_ERROR;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// print help messages
static int cmd_help (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    puts ("");
    for (int i = 0; fundefs[i].help != NULL; i ++) {
        printf ("  %10s - %s\n", fundefs[i].name, fundefs[i].help);
    }
    puts ("");
    puts ("for help on specific command, do '<command> help'");
    if (inihelp != NULL) {
        puts ("");
        puts (inihelp);
    }
    puts ("");
    return TCL_OK;
}

// set gpio pin
static int cmd_setpin (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    switch (objc) {
        case 3: {
            char const *regname = Tcl_GetString (objv[1]);
            if (strcasecmp (regname, "help") == 0) {
                puts ("");
                puts (" setpin <pinnumber> <boolean>");
                puts ("   0.. 7 : U1 GPA0..7");
                puts ("   8..15 : U1 GPB0..7");
                puts ("  16..23 : U2 GPA0..7");
                puts ("  24..31 : U2 GPB0..7");
                puts ("  32..39 : U3 GPA0..7");
                puts ("  40..47 : U3 GPB0..7");
                puts ("  48..55 : U4 GPA0..7");
                puts ("  56..63 : U4 GPB0..7");
                puts ("  64..71 : U5 GPA0..7");
                puts ("  72..79 : U5 GPB0..7");
                puts ("");
                return TCL_OK;
            }
            int pinnum, pinval;
            int rc = Tcl_GetIntFromObj (interp, objv[1], &pinnum);
            if (rc != TCL_OK) return rc;
            if ((pinnum < 0) || (pinnum > 79)) {
                Tcl_SetResultF (interp, "bad pin number %d\n", pinnum);
                return TCL_ERROR;
            }
            rc = Tcl_GetBooleanFromObj (interp, objv[2], &pinval);
            if (rc != TCL_OK) return rc;
            if (! ((wrmsks[pinnum/16] >> (pinnum % 16)) & 1)) {
                pinval = -1;
            } else if (padlib->writepin (pinnum, pinval) < 0) {
                Tcl_SetResultF (interp, "error writing pin %d\n", pinnum);
                return TCL_ERROR;
            }
            Tcl_SetObjResult (interp, Tcl_NewIntObj (pinval));
            return TCL_OK;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}

// set switch
static int cmd_setsw (ClientData clientdata, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    switch (objc) {
        case 2: {
            char const *swname = Tcl_GetString (objv[1]);
            if (strcasecmp (swname, "help") == 0) {
                puts ("");
                puts (" setsw <switchname> <integervalue>");
                for (int i = 0; permsws[i].name != NULL; i ++) {
                    printf ("      %s\n", permsws[i].name);
                }
                puts ("");
                return TCL_OK;
            }
            break;
        }
        case 3: {
            char const *swname = Tcl_GetString (objv[1]);
            int swval = 0;
            int rc = Tcl_GetIntFromObj (interp, objv[2], &swval);
            if (rc != TCL_OK) return rc;
            for (int i = 0; permsws[i].name != NULL; i ++) {
                if (strcasecmp (swname, permsws[i].name) == 0) {
                    if ((swval < 0) || (swval >= 1 << permsws[i].nbits)) {
                        Tcl_SetResultF (interp, "value %d out of range for switch %s", swval, swname);
                        return TCL_ERROR;
                    }
                    if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
                    for (int j = 0; j < permsws[i].nbits; j ++) {
                        setpin (permsws[i].pinums[j], swval & 1);
                        swval >>= 1;
                    }
                    if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();
                    return TCL_OK;
                }
            }
            Tcl_SetResultF (interp, "bad switch name %s", swname);
            return TCL_ERROR;
        }
    }
    Tcl_SetResult (interp, (char *) "bad number of arguments", TCL_STATIC);
    return TCL_ERROR;
}



/////////////////
//  Utilities  //
/////////////////

// set result to formatted string
static void Tcl_SetResultF (Tcl_Interp *interp, char const *fmt, ...)
{
    char *buf = NULL;
    va_list ap;
    va_start (ap, fmt);
    if (vasprintf (&buf, fmt, ap) < 0) ABORT ();
    va_end (ap);
    Tcl_SetResult (interp, buf, (void (*) (char *)) free);
}

// flush writes then read register
// - call with mutex locked
static uint16_t getreg (int const *pins)
{
    uint16_t reg = 0;
    for (int pin; (pin = *(pins ++)) >= 0;) {
        reg += reg + getpin (pin);
    }
    return reg;
}

// queue write for given pin
// - call with mutex locked
static void setpin (int pin, bool set)
{
    int index = pin >> 4;
    int bitno = pin & 017;
    ASSERT ((index >= 0) && (index < P_NU16S));
    uint16_t old = wrpads[index];
    if (set) wrpads[index] |= 1U << bitno;
     else wrpads[index] &= ~ (1U << bitno);
    if (old != wrpads[index]) wrpadsdirty = true;
}

// flush writes then read pin into cache if not already there
// - call with mutex locked
static bool getpin (int pin)
{
    flushit ();
    if (! rdpadsvalid) {
        padlib->readpads (rdpads);
        rdpadsvalid = true;
    }
    int index = pin >> 4;
    int bitno = pin & 017;
    ASSERT ((index >= 0) && (index < P_NU16S));
    return (rdpads[index] >> bitno) & 1;
}

// flush writes
// if anything flushed, invalidate read cache
// - call with mutex locked
static void flushit ()
{
    if (wrpadsdirty) {
        padlib->writepads (wrpads);
        wrpadsdirty = false;
        rdpadsvalid = false;
    }
}



// pass state of processor to whoever asks via udp
static void *udpthread (void *dummy)
{
    pthread_detach (pthread_self ());

    // create a socket to listen on
    int udpfd = socket (AF_INET, SOCK_DGRAM, 0);
    if (udpfd < 0) ABORT ();

    struct sockaddr_in server;
    memset (&server, 0, sizeof server);
    server.sin_family = AF_INET;
    server.sin_port   = htons (UDPPORT);
    if (bind (udpfd, (sockaddr *) &server, sizeof server) < 0) {
        fprintf (stderr, "error binding to %d: %m\n", UDPPORT);
        ABORT ();
    }

    while (true) {
        struct sockaddr_in client;
        socklen_t clilen = sizeof client;
        UDPPkt udppkt;
        int rc = recvfrom (udpfd, &udppkt, sizeof udppkt, 0, (struct sockaddr *) &client, &clilen);
        if (rc != sizeof udppkt) {
            if (rc < 0) {
                fprintf (stderr, "error receiving udp packet: %m\n");
            } else {
                fprintf (stderr, "only received %d of %d bytes\n", rc, (int) sizeof udppkt);
            }
            continue;
        }

        if (pthread_mutex_lock (&padmutex) != 0) ABORT ();
        flushit ();
        rdpadsvalid = false;
        udppkt.ma    = getreg (mabits);
        udppkt.ir    = getreg (irbits) << 9;
        udppkt.mb    = getreg (mbbits);
        udppkt.ac    = getreg (acbits);
        udppkt.sr    = getreg (srbits);
        udppkt.ea    = getpin (P_EMA);
        udppkt.stf   = getpin (P_FET);
        udppkt.ste   = getpin (P_EXE);
        udppkt.std   = getpin (P_DEF);
        udppkt.stwc  = getpin (P_WCT);
        udppkt.stca  = getpin (P_CAD);
        udppkt.stb   = getpin (P_BRK);
        udppkt.link  = getpin (P_LINK);
        udppkt.ion   = getpin (P_ION);
        udppkt.per   = getpin (P_PARE);
        udppkt.prot  = getpin (P_PRTE);
        udppkt.run   = getpin (P_RUN);
        udppkt.mprt  = getpin (P_MPRT);
        udppkt.dfld  = getpin (P_DFLD);
        udppkt.ifld  = getpin (P_IFLD);
        udppkt.ldad  = getpin (P_LDAD);
        udppkt.start = getpin (P_STRT);
        udppkt.cont  = getpin (P_CONT);
        udppkt.stop  = getpin (P_STOP);
        udppkt.step  = getpin (P_STEP);
        udppkt.exam  = getpin (P_EXAM);
        udppkt.dep   = getpin (P_DEP);
        if (pthread_mutex_unlock (&padmutex) != 0) ABORT ();

        rc = sendto (udpfd, &udppkt, sizeof udppkt, 0, (sockaddr *) &client, sizeof client);
        if (rc != sizeof udppkt) {
            if (rc < 0) {
                fprintf (stderr, "error sending udp packet: %m\n");
            } else {
                fprintf (stderr, "only sent %d of %d bytes\n", rc, (int) sizeof udppkt);
            }
        }
    }
}
