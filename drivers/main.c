/* main.c - Network UPS Tools driver core

   Copyright (C) 1999  Russell Kroll <rkroll@exploits.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "main.h"
#include "dstate.h"

	/* data which may be useful to the drivers */	
	int	upsfd = -1;
	char	*device_path = NULL;
	const	char	*progname = NULL, *upsname = NULL, 
			*device_name = NULL;

	/* may be set by the driver to wake up while in dstate_poll_fds */
	int	extrafd = -1;

	/* for ser_open */
	int	do_lock_port = 1;

	/* set by the drivers */
	int	experimental_driver = 0;
	int	broken_driver = 0;

	/* for detecting -a values that don't match anything */
	static	int	upsname_found = 0;

	static vartab_t	*vartab_h = NULL;

	/* variables possibly set by the global part of ups.conf */
	unsigned int	poll_interval = 2;
	static char	*chroot_path = NULL, *user = NULL;

	/* signal handling */
	int	exit_flag = 0;

	/* everything else */
	static	char	*pidfn = NULL;

/* power down the attached load immediately */
static void forceshutdown(void)
{
	upslogx(LOG_NOTICE, "Initiating UPS shutdown");

	/* the driver must not block in this function */
	upsdrv_shutdown();
	exit(EXIT_SUCCESS);
}

/* this function only prints the usage message; it does not call exit() */
static void help_msg(void)
{
	vartab_t	*tmp;

	printf("\nusage: %s -a <id> [OPTIONS]\n", progname);

	printf("  -a <id>        - autoconfig using ups.conf section <id>\n");
	printf("                 - note: -x after -a overrides ups.conf settings\n\n");

	printf("  -V             - print version, then exit\n");
	printf("  -L             - print parseable list of driver variables\n");
	printf("  -D             - raise debugging level\n");
	printf("  -h             - display this help\n");
	printf("  -k             - force shutdown\n");
	printf("  -i <int>       - poll interval\n");
	printf("  -r <dir>       - chroot to <dir>\n");
	printf("  -u <user>      - switch to <user> (if started as root)\n");
	printf("  -x <var>=<val> - set driver variable <var> to <val>\n");
	printf("                 - example: -x cable=940-0095B\n\n");

	if (vartab_h) {
		tmp = vartab_h;

		printf("Acceptable values for -x or ups.conf in this driver:\n\n");

		while (tmp) {
			if (tmp->vartype == VAR_VALUE)
				printf("%40s : -x %s=<value>\n", 
					tmp->desc, tmp->var);
			else
				printf("%40s : -x %s\n", tmp->desc, tmp->var);
			tmp = tmp->next;
		}
	}

	upsdrv_help();
}

/* store these in dstate as driver.(parameter|flag) */
static void dparam_setinfo(const char *var, const char *val)
{
	char	vtmp[SMALLBUF];

	/* store these in dstate for debugging and other help */
	if (val) {
		snprintf(vtmp, sizeof(vtmp), "driver.parameter.%s", var);
		dstate_setinfo(vtmp, "%s", val);
		return;
	}

	/* no value = flag */

	snprintf(vtmp, sizeof(vtmp), "driver.flag.%s", var);
	dstate_setinfo(vtmp, "enabled");
}

/* cram var [= <val>] data into storage */
static void storeval(const char *var, char *val)
{
	vartab_t	*tmp, *last;

	if (!strncasecmp(var, "override.", 9)) {
		dstate_setinfo(var+9, "%s", val);
		dstate_setflags(var+9, ST_FLAG_IMMUTABLE);
		return;
	}

	if (!strncasecmp(var, "default.", 8)) {
		dstate_setinfo(var+8, "%s", val);
		return;
	}

	tmp = last = vartab_h;

	while (tmp) {
		last = tmp;

		/* sanity check */
		if (!tmp->var) {
			tmp = tmp->next;
			continue;
		}

		/* later definitions overwrite earlier ones */
		if (!strcasecmp(tmp->var, var)) {
			free(tmp->val);

			if (val)
				tmp->val = xstrdup(val);

			/* don't keep things like SNMP community strings */
			if ((tmp->vartype & VAR_SENSITIVE) == 0)
				dparam_setinfo(var, val);

			tmp->found = 1;
			return;
		}

		tmp = tmp->next;
	}

	/* try to help them out */
	printf("\nFatal error: '%s' is not a valid %s for this driver.\n", var,
		val ? "variable name" : "flag");
	printf("\n");
	printf("Look in the man page or call this driver with -h for a list of\n");
	printf("valid variable names and flags.\n");

	exit(EXIT_SUCCESS);
}

/* retrieve the value of variable <var> if possible */
char *getval(const char *var)
{
	vartab_t	*tmp = vartab_h;

	while (tmp) {
		if (!strcasecmp(tmp->var, var))
			return(tmp->val);
		tmp = tmp->next;
	}

	return NULL;
}

/* see if <var> has been defined, even if no value has been given to it */
int testvar(const char *var)
{
	vartab_t	*tmp = vartab_h;

	while (tmp) {
		if (!strcasecmp(tmp->var, var))
			return tmp->found;
		tmp = tmp->next;
	}

	return 0;	/* not found */
}

/* callback from driver - create the table for -x/conf entries */
void addvar(int vartype, const char *name, const char *desc)
{
	vartab_t	*tmp, *last;

	tmp = last = vartab_h;

	while (tmp) {
		last = tmp;
		tmp = tmp->next;
	}

	tmp = xmalloc(sizeof(vartab_t));

	tmp->vartype = vartype;
	tmp->var = xstrdup(name);
	tmp->val = NULL;
	tmp->desc = xstrdup(desc);
	tmp->found = 0;
	tmp->next = NULL;

	if (last)
		last->next = tmp;
	else
		vartab_h = tmp;
}	

/* handle -x / ups.conf config details that are for this part of the code */
static int main_arg(char *var, char *val)
{
	/* flags for main: just 'nolock' for now */

	if (!strcmp(var, "nolock")) {
		do_lock_port = 0;
		dstate_setinfo("driver.flag.nolock", "enabled");
		return 1;	/* handled */
	}

	/* any other flags are for the driver code */
	if (!val)
		return 0;

	/* variables for main: port */

	if (!strcmp(var, "port")) {
		device_path = xstrdup(val);
		device_name = xbasename(device_path);
		dstate_setinfo("driver.parameter.port", "%s", val);
		return 1;	/* handled */
	}

	if (!strcmp(var, "sddelay")) {
		upslogx(LOG_INFO, "Obsolete value sddelay found in ups.conf");
		return 1;	/* handled */
	}

	/* only for upsdrvctl - ignored here */
	if (!strcmp(var, "sdorder"))
		return 1;	/* handled */

	/* only for upsd (at the moment) - ignored here */
	if (!strcmp(var, "desc"))
		return 1;	/* handled */

	return 0;	/* unhandled, pass it through to the driver */
}

static void do_global_args(const char *var, const char *val)
{
	if (!strcmp(var, "pollinterval")) {
		poll_interval = atoi(val);
		return;
	}

	if (!strcmp(var, "chroot")) {
		free(chroot_path);
		chroot_path = xstrdup(val);
	}

	if (!strcmp(var, "user")) {
		free(user);
		user = xstrdup(val);
	}


	/* unrecognized */
}

void do_upsconf_args(char *confupsname, char *var, char *val)
{
	char	tmp[SMALLBUF];

	/* handle global declarations */
	if (!confupsname) {
		do_global_args(var, val);
		return;
	}

	/* no match = not for us */
	if (strcmp(confupsname, upsname) != 0)
		return;

	upsname_found = 1;

	if (main_arg(var, val))
		return;

	/* flags (no =) now get passed to the driver-level stuff */
	if (!val) {

		/* also store this, but it's a bit different */
		snprintf(tmp, sizeof(tmp), "driver.flag.%s", var);
		dstate_setinfo(tmp, "enabled");

		storeval(var, NULL);
		return;
	}

	/* don't let the user shoot themselves in the foot */
	if (!strcmp(var, "driver")) {
		if (strcmp(val, progname) != 0)
			fatalx(EXIT_FAILURE, "Error: UPS [%s] is for driver %s, but I'm %s!\n",
				confupsname, val, progname);
		return;
	}

	/* allow per-driver overrides of the global setting */
	if (!strcmp(var, "pollinterval")) {
		poll_interval = atoi(val);
		return;
	}

	/* everything else must be for the driver */

	storeval(var, val);
}

/* split -x foo=bar into 'foo' and 'bar' */
static void splitxarg(char *inbuf)
{
	char	*eqptr, *val, *buf;

	/* make our own copy - avoid changing argv */
	buf = xstrdup(inbuf);

	eqptr = strchr(buf, '=');

	if (!eqptr)
		val = NULL;
	else {
		*eqptr++ = '\0';
		val = eqptr;
	}

	/* see if main handles this first */
	if (main_arg(buf, val))
		return;

	/* otherwise store it for later */
	storeval(buf, val);
}

/* dump the list from the vartable for external parsers */
static void listxarg(void)
{
	vartab_t	*tmp;

	tmp = vartab_h;

	if (!tmp)
		return;

	while (tmp) {

		switch (tmp->vartype) {
			case VAR_VALUE: printf("VALUE"); break;
			case VAR_FLAG: printf("FLAG"); break;
			default: printf("UNKNOWN"); break;
		}

		printf(" %s \"%s\"\n", tmp->var, tmp->desc);

		tmp = tmp->next;
	}
}

static void vartab_free(void)
{
	vartab_t	*tmp, *next;

	tmp = vartab_h;

	while (tmp) {
		next = tmp->next;

		free(tmp->var);
		free(tmp->val);
		free(tmp->desc);
		free(tmp);

		tmp = next;
	}
}

static void exit_cleanup(void)
{
	free(chroot_path);
	free(device_path);
	free(user);

	if (pidfn) {
		unlink(pidfn);
		free(pidfn);
	}

	dstate_free();
	vartab_free();
}

static void set_exit_flag(int sig)
{
	exit_flag = sig;
}

static void setup_signals(void)
{
	struct sigaction	sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sa.sa_handler = set_exit_flag;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	sa.sa_handler = SIG_IGN;
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
}

int main(int argc, char **argv)
{
	struct	passwd	*new_uid = NULL;
	int	i, do_forceshutdown = 0;

	atexit(exit_cleanup);

	/* pick up a default from configure --with-user */
	user = xstrdup(RUN_AS_USER);	/* xstrdup: this gets freed at exit */

	progname = xbasename(argv[0]);
	open_syslog(progname);

	upsdrv_banner();

	if (experimental_driver) {
		printf("Warning: This is an experimental driver.\n");
		printf("Some features may not function correctly.\n\n");
	}

	/* build the driver's extra (-x) variable table */
	upsdrv_makevartable();

	while ((i = getopt(argc, argv, "+a:kDhx:Lr:u:Vi:")) != -1) {
		switch (i) {
			case 'a':
				upsname = optarg;

				read_upsconf();

				if (!upsname_found)
					fatalx(EXIT_FAILURE, "Error: Section %s not found in ups.conf",
						optarg);
				break;
			case 'D':
				nut_debug_level++;
				break;
			case 'i':
				poll_interval = atoi(optarg);
				break;
			case 'k':
				do_lock_port = 0;
				do_forceshutdown = 1;
				break;
			case 'L':
				listxarg();
				exit(EXIT_SUCCESS);
			case 'r':
				chroot_path = xstrdup(optarg);
				break;
			case 'u':
				user = xstrdup(optarg);
				break;
			case 'V':
				/* already printed the banner, so exit */
				exit(EXIT_SUCCESS);
			case 'x':
				splitxarg(optarg);
				break;
			case 'h':
				help_msg();
				exit(EXIT_SUCCESS);
			default:
				fatalx(EXIT_FAILURE,
					"Error: unknown option -%c. Try -h for help.", i);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fatalx(EXIT_FAILURE,
			"Error: too many non-option arguments. Try -h for help.");
	}

	if (!upsname_found) {
		fatalx(EXIT_FAILURE,
			"Error: specifying '-a id' is now mandatory. Try -h for help.");
	}

	/* we need to get the port from somewhere */
	if (!device_path) {
		fatalx(EXIT_FAILURE,
			"Error: you must specify a port name in ups.conf. Try -h for help.");
	}

	pidfn = xmalloc(SMALLBUF);

	snprintf(pidfn, SMALLBUF, "%s/%s-%s.pid", altpidpath(), progname, upsname);

	upsdebugx(1, "debug level is '%d'", nut_debug_level);

	new_uid = get_user_pwent(user);
	
	if (chroot_path)
		chroot_start(chroot_path);

	become_user(new_uid);

	/* Only switch to statepath if we're not powering off */
	/* This avoid case where ie /var is umounted */
	if ((!do_forceshutdown) && (chdir(dflt_statepath())))
		fatal_with_errno(EXIT_FAILURE, "Can't chdir to %s", dflt_statepath());

	setup_signals();

	/* clear out callback handler data */
	memset(&upsh, '\0', sizeof(upsh));

	upsdrv_initups();

	/* UPS is detected now, cleanup upon exit */
	atexit(upsdrv_cleanup);

	/* now see if things are very wrong out there */
	if (broken_driver) {
		fatalx(EXIT_FAILURE,
			"Fatal error: broken driver. It probably needs to be converted.\n"
			"Search for 'broken_driver = 1' in the source for more details.");
	}

	if (do_forceshutdown)
		forceshutdown();

	/* get the base data established before allowing connections */
	upsdrv_initinfo();
	upsdrv_updateinfo();

	/* now we can start servicing requests */
	dstate_init(progname, upsname);

	/* publish the top-level data: version number, driver name */
	dstate_setinfo("driver.version", "%s", UPS_VERSION);
	dstate_setinfo("driver.name", "%s", progname);

	/* The poll_interval may have been changed from the default */
	dstate_setinfo("driver.parameter.pollinterval", "%d", poll_interval);

	if (nut_debug_level == 0) {
		background();
		writepid(pidfn);
	}

	while (!exit_flag) {

		struct timeval	timeout;
	
		gettimeofday(&timeout, NULL);
		timeout.tv_sec += poll_interval;

		upsdrv_updateinfo();

		while (!dstate_poll_fds(timeout, extrafd) && !exit_flag) {
			/* repeat until time is up or extrafd has data */
		}
	}

	/* if we get here, the exit flag was set by a signal handler */
	upslogx(LOG_INFO, "Signal %d: exiting", exit_flag);

	exit(EXIT_SUCCESS);
}
