/*
 * status.h — Working tree status report.
 */

#ifndef STATUS_H
#define STATUS_H

/*
 * Print what the next commit would change (index vs HEAD), what is
 * modified but unstaged (working tree vs index), and which files are
 * untracked. Returns 0 on success, -1 on error.
 */
int status_run(void);

#endif /* STATUS_H */
