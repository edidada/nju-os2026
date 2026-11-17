#define main sperf_entry
#include "../sperf/sperf.c"
#undef main
#include <assert.h>
int main(void) {
    char name[64]; double t;
    char a[] = "write(1, \"<99999.9>\", 9) = 9 <0.000123>\n";
    assert(parse_strace_line(a,name,&t) && !strcmp(name,"write") && fabs(t-.000123)<1e-12);
    char b[] = "<... read resumed>\"x\", 1) = 1 <0.25>";
    assert(parse_strace_line(b,name,&t) && !strcmp(name,"read") && t==.25);
    char *bad[] = {"+++ exited with 0 +++", "read(0) = 0 <nan>", "read(0) = 0 <-1>",
        "read(0) = 0 <1> garbage", "read(0) = 0", "strace: error <1>", "read(0) = 0 <1e999>"};
    for (size_t i=0;i<sizeof bad/sizeof bad[0];++i) assert(!parse_strace_line(bad[i],name,&t));
    syscall_stats s={0};add_syscall(&s,"read",1);add_syscall(&s,"write",2);add_syscall(&s,"read",3);
    assert(s.count==2 && s.total_time==6 && s.stats[0].time==4);
    print_top_syscalls(&s,5);
    return 0;
}
