#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <testkit.h>
#include "labyrinth.h"

bool isValidPlayer(char id) { return id >= '0' && id <= '9'; }
static bool inside(Labyrinth *lab, int r, int c) {
    return r >= 0 && c >= 0 && r < lab->rows && c < lab->cols;
}
bool isEmptySpace(Labyrinth *lab, int r, int c) {
    return inside(lab, r, c) && lab->map[r][c] == '.';
}
Position findPlayer(Labyrinth *lab, char id) {
    for (int r=0; r<lab->rows; ++r)
        for (int c=0; c<lab->cols; ++c)
            if (lab->map[r][c] == id) return (Position){r,c};
    return (Position){-1,-1};
}
Position findFirstEmptySpace(Labyrinth *lab) { return findPlayer(lab, '.'); }

bool loadMap(Labyrinth *lab, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return false;
    Labyrinth next = {0};
    bool players[10] = {0};
    int ch, col=0;
    bool ok=true;
    while ((ch=fgetc(f)) != EOF) {
        if (ch == '\n') {
            if (col == 0 || (next.rows && col != next.cols)) { ok=false; break; }
            next.cols=col; ++next.rows; col=0;
        } else {
            if (next.rows >= MAX_ROWS || col >= MAX_COLS ||
                (ch != '#' && ch != '.' && !isValidPlayer(ch))) { ok=false; break; }
            if (isValidPlayer(ch)) {
                if (players[ch-'0']) { ok=false; break; }
                players[ch-'0']=true;
            }
            next.map[next.rows][col++]=(char)ch;
        }
    }
    if (ferror(f)) ok=false;
    fclose(f);
    if (col) {
        if (next.rows && col != next.cols) ok=false;
        next.cols=col; ++next.rows;
    }
    if (!ok || !next.rows || !next.cols) return false;
    *lab=next;
    return true;
}

void dfs(Labyrinth *lab, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    Position stack[MAX_ROWS*MAX_COLS];
    int size=0;
    if (!inside(lab,row,col) || lab->map[row][col]=='#' || visited[row][col]) return;
    visited[row][col]=true;
    stack[size++]=(Position){row,col};
    while (size) {
        Position p=stack[--size];
        const int dr[4]={-1,1,0,0}, dc[4]={0,0,-1,1};
        for (int d=0; d<4; ++d) {
            int r=p.row+dr[d], c=p.col+dc[d];
            if (inside(lab,r,c) && lab->map[r][c]!='#' && !visited[r][c]) {
                visited[r][c]=true;
                stack[size++]=(Position){r,c};
            }
        }
    }
}
bool isConnected(Labyrinth *lab) {
    bool visited[MAX_ROWS][MAX_COLS]={0};
    for (int r=0; r<lab->rows; ++r) for (int c=0; c<lab->cols; ++c) {
        if (lab->map[r][c]!='#') {
            dfs(lab,r,c,visited);
            for (int i=0;i<lab->rows;++i) for (int j=0;j<lab->cols;++j)
                if (lab->map[i][j]!='#' && !visited[i][j]) return false;
            return true;
        }
    }
    return true;
}
bool movePlayer(Labyrinth *lab, char id, const char *direction) {
    if (!isValidPlayer(id)) return false;
    int dr=0, dc=0;
    if (!strcmp(direction,"up")) dr=-1;
    else if (!strcmp(direction,"down")) dr=1;
    else if (!strcmp(direction,"left")) dc=-1;
    else if (!strcmp(direction,"right")) dc=1;
    else return false;
    Position p=findPlayer(lab,id);
    if (p.row < 0) p=findFirstEmptySpace(lab);
    if (p.row < 0 || !isEmptySpace(lab,p.row+dr,p.col+dc)) return false;
    lab->map[p.row][p.col]='.';
    lab->map[p.row+dr][p.col+dc]=id;
    return true;
}
bool saveMap(Labyrinth *lab, const char *filename) {
    size_t length=strlen(filename)+16;
    char *temp=malloc(length);
    if (!temp) return false;
    snprintf(temp,length,"%s.XXXXXX",filename);
    int fd=mkstemp(temp);
    if (fd < 0) { free(temp); return false; }
    FILE *f=fdopen(fd,"w");
    if (!f) { close(fd); unlink(temp); free(temp); return false; }
    bool ok=true;
    for (int r=0;r<lab->rows;++r) {
        if (fwrite(lab->map[r],1,lab->cols,f)!=(size_t)lab->cols || fputc('\n',f)==EOF) ok=false;
    }
    if (fclose(f)) ok=false;
    if (ok && rename(temp,filename)) ok=false;
    if (!ok) unlink(temp);
    free(temp);
    return ok;
}
int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--version")) { puts(VERSION_INFO); return 0; }
    const char *filename=NULL, *player=NULL, *direction=NULL;
    for (int i=1;i<argc;++i) {
        const char **target=NULL;
        if (!strcmp(argv[i],"-m") || !strcmp(argv[i],"--map")) target=&filename;
        else if (!strcmp(argv[i],"-p") || !strcmp(argv[i],"--player")) target=&player;
        else if (!strcmp(argv[i],"--move")) target=&direction;
        else return 1;
        if (*target || ++i>=argc) return 1;
        *target=argv[i];
    }
    if (!filename || !player || strlen(player)!=1 || !isValidPlayer(*player)) return 1;
    Labyrinth lab;
    if (!loadMap(&lab,filename) || !isConnected(&lab)) return 1;
    if (direction) return movePlayer(&lab,*player,direction) && saveMap(&lab,filename) ? 0 : 1;
    FILE *f=fopen(filename,"rb");
    if (!f) return 1;
    int ch;
    while ((ch=fgetc(f))!=EOF) if (putchar(ch)==EOF) { fclose(f); return 1; }
    bool ok=!ferror(f);
    fclose(f);
    return ok ? 0 : 1;
}
