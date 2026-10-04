/*
 * Auto-mount daemon and its launchd setup.
 */
#ifndef N4M_DAEMON_H
#define N4M_DAEMON_H

/* Runs forever, needs root. */
int daemon_main(int argc, char **argv);

/* sudo ntfs4mac install / uninstall */
int install_main(void);
int uninstall_main(void);

#endif /* N4M_DAEMON_H */
