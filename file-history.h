#ifndef FILE_HISTORY_H
#define FILE_HISTORY_H

void add_file_history(int row, int col, const char *filename);
void load_file_history(void);
void save_file_history(void);
int find_file_in_history(const char *filename, int *row, int *col);

#endif
