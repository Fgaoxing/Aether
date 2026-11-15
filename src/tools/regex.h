/* regex.h — 精简扩展正则引擎（grep 用） */
#ifndef AETHER_REGEX_H
#define AETHER_REGEX_H

/* 编译 pattern（NUL 结尾）；ci=1 忽略大小写。0 成功，-1 失败（语法错误/节点池满） */
int re_compile(const char *pattern, int ci);
/* 在 s[0..slen-1] 中搜索；1 有匹配，0 无匹配（需先 re_compile） */
int re_search(const char *s, int slen);

#endif /* AETHER_REGEX_H */
