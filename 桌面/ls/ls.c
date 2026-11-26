#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pwd.h>          // getpwuid：把用户ID变成用户名
#include <grp.h>          // getgrgid：把组ID变成组名
#include <limits.h>       // PATH_MAX：路径缓冲区最大长度
#include <sys/sysmacros.h> // major/minor：设备文件的主次设备号

// 半年大约是多少秒。GNU ls 用它判断一个文件是不是“老文件”
#define SIX_MONTHS (31556952 / 2)

// 程序名，报错时显示，比如 "ls: cannot access ..."
const char *program_name = "ls";

// 排序方式：按名字，或者按修改时间
enum sort_key { SORT_NAME, SORT_TIME };

// 所有命令行开关都放在这个结构体里，函数之间只传一个指针
struct ls_options {
    int all;          // -a：显示隐藏文件
    int inode;        // -i：显示 inode 号
    int long_fmt;     // -l：显示详细信息
    int blocks;       // -s：显示占用块数
    int recursive;    // -R：递归列出子目录
    int reverse;      // -r：反向排序
    enum sort_key sort; // -t：按时间排序；默认按名字
};

// 一个文件的基本信息
struct file_info {
    char *name;       // 文件名（目录里的名字，或者命令行上的路径）
    ino_t inode;      // inode 号
    struct stat st;   // stat 拿到的详细信息
};

// 命令行操作数的收集结果
struct operand_list {
    struct file_info *items; // 存所有操作数的数组，调用方负责 free
    int count;               // 成功 lstat 的条数
    int total;               // 命令行上一共给了几个操作数
    int errors;              // lstat 失败的个数
};

// ==================== 小工具函数 ====================

// 把用户ID变成用户名。查不到就显示数字，和 GNU ls 一样
static void uid_to_name(uid_t uid, char *buf, size_t size)
{
    struct passwd *pw = getpwuid(uid);   // 去系统用户表里查
    if (pw != NULL)
        snprintf(buf, size, "%s", pw->pw_name); // 查到了，复制用户名
    else
        snprintf(buf, size, "%u", (unsigned)uid); // 查不到，显示数字
}

// 把组ID变成组名。查不到就显示数字
static void gid_to_name(gid_t gid, char *buf, size_t size)
{
    struct group *gr = getgrgid(gid);    // 去系统组表里查
    if (gr != NULL)
        snprintf(buf, size, "%s", gr->gr_name); // 查到了，复制组名
    else
        snprintf(buf, size, "%u", (unsigned)gid); // 查不到，显示数字
}

// 把“文件大小”变成字符串
// 普通文件显示字节数；设备文件显示“主设备号, 次设备号”
static void size_to_str(const struct stat *st, char *buf, size_t size)
{
    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
        snprintf(buf, size, "%u, %u", major(st->st_rdev), minor(st->st_rdev));
    else
        snprintf(buf, size, "%ld", (long)st->st_size);
}

// 每一列要占多宽。先量一遍，再打印，这样列才对得齐
struct col_widths {
    int inode;   // inode 列宽度
    int blocks;  // 块数列宽度
    int nlink;   // 硬链接数列宽度
    int owner;   // 用户名列宽度
    int group;   // 组名列宽度
    int size;    // 大小列宽度
};

// 遍历所有条目，算出每一列的最大宽度
static struct col_widths compute_widths(const struct file_info *files, int count,
                                        const struct ls_options *opt)
{
    struct col_widths w = {0};   // 全部先设为 0
    char buf[256];               // 临时存放格式化后的文字

    for (int i = 0; i < count; i++)
    {
        const struct stat *st = &files[i].st;
        int n; // snprintf 的返回值，正好就是字符串长度

        if (opt->inode)          // 如果开了 -i，才需要 inode 列
        {
            n = snprintf(buf, sizeof buf, "%lu", (unsigned long)files[i].inode);
            if (n > w.inode) w.inode = n; // 记录最大宽度
        }

        if (opt->blocks)         // 如果开了 -s，才需要块数列
        {
            n = snprintf(buf, sizeof buf, "%lu",
                         (unsigned long)((st->st_blocks + 1) / 2));
            if (n > w.blocks) w.blocks = n;
        }

        n = snprintf(buf, sizeof buf, "%lu", (unsigned long)st->st_nlink);
        if (n > w.nlink) w.nlink = n; // 硬链接数最大几位

        uid_to_name(st->st_uid, buf, sizeof buf);
        n = (int)strlen(buf);
        if (n > w.owner) w.owner = n; // 用户名最长几个字符

        gid_to_name(st->st_gid, buf, sizeof buf);
        n = (int)strlen(buf);
        if (n > w.group) w.group = n; // 组名最长几个字符

        size_to_str(st, buf, sizeof buf);
        n = (int)strlen(buf);
        if (n > w.size) w.size = n;   // 大小最长几个字符
    }

    return w; // 把六列宽度一起返回
}

// 把权限位变成 "drwxr-xr-x" 这样的字符串
void mode_to_str(mode_t mode, char *buf)
{
    // 第 0 位：文件类型
    if (S_ISDIR(mode))
        buf[0] = 'd';            // 目录
    else if (S_ISLNK(mode))
        buf[0] = 'l';            // 符号链接
    else if (S_ISCHR(mode))
        buf[0] = 'c';            // 字符设备，比如 /dev/null
    else if (S_ISBLK(mode))
        buf[0] = 'b';            // 块设备，比如 /dev/sda
    else if (S_ISFIFO(mode))
        buf[0] = 'p';            // 管道文件
    else if (S_ISSOCK(mode))
        buf[0] = 's';            // socket 文件
    else
        buf[0] = '-';            // 普通文件

    // 第 1~9 位：用户、组、其他人的 rwx
    const char *perms = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
    {
        if (mode & (1 << (8 - i))) // 这一位是 1，说明有这个权限
            buf[1 + i] = perms[i];
        else
            buf[1 + i] = '-';      // 没有这个权限
    }

    // 特殊权限：setuid、setgid、sticky
    // 有执行位显示小写 s/t，没有执行位显示大写 S/T
    if (mode & S_ISUID)
        buf[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID)
        buf[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX)
        buf[9] = (mode & S_IXOTH) ? 't' : 'T';

    buf[10] = '\0'; // 字符串结尾
}

// 按名字排序（正序）
int compare_name(const void *a, const void *b)
{
    struct file_info *filesA = (struct file_info *)a;
    struct file_info *filesB = (struct file_info *)b;
    return strcmp(filesA->name, filesB->name); // 名字小的排前面
}

// 按修改时间排序：新的在前，旧的在后
int compare_mtime(const void *a, const void *b)
{
    struct file_info *filesA = (struct file_info *)a;
    struct file_info *filesB = (struct file_info *)b;

    // 先比秒，再比纳秒，这样同一秒内创建的文件也能分清先后
    if (filesA->st.st_mtim.tv_sec < filesB->st.st_mtim.tv_sec)
        return 1;  // A 更旧，A 往后放
    if (filesA->st.st_mtim.tv_sec > filesB->st.st_mtim.tv_sec)
        return -1; // A 更新，A 往前放
    if (filesA->st.st_mtim.tv_nsec < filesB->st.st_mtim.tv_nsec)
        return 1;
    if (filesA->st.st_mtim.tv_nsec > filesB->st.st_mtim.tv_nsec)
        return -1;
    return 0; // 时间完全一样：保持目录读出来的先后顺序
}

// 把整个数组倒过来
// -r 不是“换一种比较方法”，而是“排好序之后整体倒过来”，
// 这样连时间相同的文件，顺序也会跟着反过来，和 GNU ls 一致
static void reverse_files(struct file_info *files, int count)
{
    for (int i = 0; i < count / 2; i++)
    {
        struct file_info temp = files[i];
        files[i] = files[count - 1 - i];
        files[count - 1 - i] = temp;
    }
}

// 打印一批文件信息
// base 是“这些条目所在的目录路径”。目录里的条目需要它来拼完整路径；
// 命令行上的文件操作数传 NULL，因为它的名字本身就是路径
void do_print(struct file_info *files, int count, const struct ls_options *opt,
              const char *base)
{
    struct col_widths w = compute_widths(files, count, opt); // 先量好列宽
    time_t now = time(NULL); // 当前时间只取一次，所有文件共用

    for (int i = 0; i < count; i++)
    {
        struct stat *st = &files[i].st;

        if (opt->inode) // -i：先打印 inode 号
        {
            printf("%*lu ", w.inode, (unsigned long)files[i].inode);
        }

        if (opt->blocks) // -s：打印占用块数（每块按 1K 算）
        {
            printf("%*lu ", w.blocks,
                   (unsigned long)((st->st_blocks + 1) / 2));
        }

        if (opt->long_fmt) // -l：打印详细信息
        {
            char mode_buf[11];                 // 权限字符串
            mode_to_str(st->st_mode, mode_buf);

            char owner[256], group[256], size_buf[256];
            uid_to_name(st->st_uid, owner, sizeof owner);   // 用户名
            gid_to_name(st->st_gid, group, sizeof group);   // 组名
            size_to_str(st, size_buf, sizeof size_buf);     // 大小

            // 时间：半年内的文件显示“月 日 时:分”，老文件显示“月 日 年”
            char time_buf[64];
            struct tm *tm_info = localtime(&st->st_mtime);
            if (st->st_mtime > now + 3600 || now - st->st_mtime > SIX_MONTHS)
                strftime(time_buf, sizeof time_buf, "%b %e  %Y", tm_info);
            else
                strftime(time_buf, sizeof time_buf, "%b %e %H:%M", tm_info);

            // 按列宽打印：数字右对齐，用户名/组名左对齐
            printf("%s %*lu %-*s %-*s %*s %s ",
                   mode_buf,
                   w.nlink, (unsigned long)st->st_nlink,
                   w.owner, owner,
                   w.group, group,
                   w.size, size_buf,
                   time_buf);
        }

        printf("%s", files[i].name); // 打印文件名

        // 只有 -l 才在符号链接后面显示 " -> 目标"
        if (opt->long_fmt && S_ISLNK(st->st_mode))
        {
            char link_path[PATH_MAX];
            if (base != NULL) // 目录里的条目：拼成 目录/名字
                snprintf(link_path, sizeof link_path, "%s/%s", base, files[i].name);
            else              // 命令行操作数：名字本身就是路径
                snprintf(link_path, sizeof link_path, "%s", files[i].name);

            char target[PATH_MAX];
            ssize_t n = readlink(link_path, target, sizeof target - 1);
            if (n >= 0) // 读到了链接指向哪里
            {
                target[n] = '\0'; // readlink 不会自动加结尾，要自己补
                printf(" -> %s", target);
            }
        }

        printf("\n"); // 一个条目打印完，换行
    }
}

// 列出 path 这个目录里的内容
// show_header 表示“要不要打印 路径: 这个标题”
// blank_before 表示“标题前面要不要先空一行”（多个操作数时用）
int list_dir(const char *path, int depth, const struct ls_options *opt,
             int show_header, int blank_before)
{
    DIR *dp = opendir(path); // 打开目录
    if (dp == NULL)
    {
        // 打不开就报告原因。命令行上的目录算严重错误 2，子目录算 1
        fprintf(stderr, "%s: cannot open directory '%s': %s\n",
                program_name, path, strerror(errno));
        return (depth == 0) ? 2 : 1;
    }

    // 目录真的打开成功了，才打印 "路径:" 标题
    if (show_header)
    {
        if (blank_before)
            printf("\n");
        printf("%s:\n", path);
    }

    int status = 0;          // 这个目录里有没有出错
    struct file_info *files; // 用来存目录里的所有条目
    int count = 0;           // 已经存了几个
    int capacity = 64;       // 数组现在能装几个
    struct dirent *entry;    // readdir 的返回值

    files = malloc(capacity * sizeof(struct file_info)); // 先分配 64 个位置
    if (files == NULL)
    {
        perror("malloc");
        closedir(dp);
        return 1;
    }

    while ((entry = readdir(dp)) != NULL) // 一次读一个目录项
    {
        // 没开 -a 时，跳过以 '.' 开头的隐藏文件
        if (opt->all == 0 && entry->d_name[0] == '.')
            continue;

        // 数组装满了，就扩容成原来的两倍
        if (count == capacity)
        {
            capacity *= 2;
            struct file_info *temp = realloc(files, capacity * sizeof(struct file_info));
            if (temp == NULL)
            {
                perror("realloc");
                closedir(dp);
                return 1;
            }
            files = temp;
        }

        files[count].name = strdup(entry->d_name); // 复制文件名
        if (files[count].name == NULL)
        {
            perror("strdup");
            status = 1; // 记录一下：这个目录处理出错了
            break;
        }
        files[count].inode = entry->d_ino; // readdir 自带的 inode 号

        // 拼出完整路径，才能 lstat
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        // lstat 不跟随符号链接，拿到的是条目自己的信息
        if (lstat(full_path, &files[count].st) != 0)
        {
            free(files[count].name); // 拿不到信息，就不要这个条目
            continue;
        }
        count++;
    }
    closedir(dp); // 目录读完了，关掉

    // 先按名字或时间排好序
    if (opt->sort == SORT_TIME)
        qsort(files, count, sizeof(struct file_info), compare_mtime);
    else
        qsort(files, count, sizeof(struct file_info), compare_name);

    // 如果开了 -r，再把整个数组倒过来
    if (opt->reverse)
        reverse_files(files, count);

    // -l 或 -s 都要打印 total（这个目录一共占多少块）
    if (opt->blocks || opt->long_fmt)
    {
        unsigned long total = 0;
        for (int i = 0; i < count; i++)
        {
            total += (unsigned long)files[i].st.st_blocks; // 先按 512B 累加
        }
        printf("total %lu\n", (total + 1) / 2); // 最后整体换算成 1K 块
    }

    do_print(files, count, opt, path); // 打印这个目录的所有条目

    // 如果开了 -R，还要进入每一个子目录
    for (int i = 0; i < count; i++)
    {
        if (opt->recursive)
        {
            // 是目录，而且不是 . 和 ..，就递归
            if (S_ISDIR(files[i].st.st_mode) &&
                (strcmp(files[i].name, ".") != 0) &&
                (strcmp(files[i].name, "..") != 0))
            {
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i].name);
                // 递归进去；子目录标题由 list_dir 在打开成功后打印
                int rc = list_dir(full_path, depth + 1, opt, 1, 1);
                if (rc > status)
                    status = rc; // 记录子目录里的错误
            }
        }
        free(files[i].name); // 名字用完了，释放
    }

    free(files); // 数组也用完了，释放
    return status; // 把这个目录的错误码交回上一层
}

// 把命令行剩下的参数收集成操作数列表
static struct operand_list build_operand_list(char **argv, int argc, int optind,
                                              const struct ls_options *opt)
{
    struct operand_list list = {0}; // 全部先清零
    int noperands = argc - optind;  // getopt 解析完后，剩下几个参数

    // 没有操作数时，默认列当前目录 "."
    list.total = (noperands > 0) ? noperands : 1;

    list.items = calloc(list.total, sizeof(struct file_info)); // 每个操作数一个位置
    if (list.items == NULL)
        return list; // items 是 NULL，说明内存不够

    for (int i = 0; i < list.total; i++)
    {
        const char *path = (noperands > 0) ? argv[optind + i] : ".";
        struct stat st_entry, st_target;

        if (lstat(path, &st_entry) != 0) // 先看操作数自己是什么
        {
            fprintf(stderr, "%s: cannot access '%s': %s\n",
                    program_name, path, strerror(errno));
            list.errors++;
            continue; // 这个操作数失败，继续处理下一个
        }

        // 普通目录：要展开它里面的内容
        int list_contents = S_ISDIR(st_entry.st_mode);

        // 符号链接指向目录，而且没开 -l：也展开目录内容
        if (!list_contents && S_ISLNK(st_entry.st_mode) && !opt->long_fmt)
        {
            if (stat(path, &st_target) == 0 && S_ISDIR(st_target.st_mode))
                list_contents = 1;
        }

        list.items[list.count].name = (char *)path; // 名字就用命令行原文
        list.items[list.count].inode = st_entry.st_ino;
        list.items[list.count].st =
            (list_contents && S_ISLNK(st_entry.st_mode)) ? st_target : st_entry;
        list.count++;
    }

    return list;
}

int main(int argc, char *argv[])
{
    // 取程序名，报错时显示。比如 ./myls 就显示 myls
    const char *slash = strrchr(argv[0], '/');
    program_name = (slash != NULL) ? slash + 1 : argv[0];

    struct ls_options opt = {0}; // 所有开关一开始都是关的
    int c;

    // getopt 会把 -al 自动拆成 -a 和 -l
    while ((c = getopt(argc, argv, "ailRrst")) != -1)
    {
        switch (c)
        {
            case 'a': opt.all = 1; break;          // -a
            case 'i': opt.inode = 1; break;        // -i
            case 'l': opt.long_fmt = 1; break;     // -l
            case 'R': opt.recursive = 1; break;    // -R
            case 'r': opt.reverse = 1; break;      // -r
            case 's': opt.blocks = 1; break;       // -s
            case 't': opt.sort = SORT_TIME; break; // -t
            default:
                return 2; // 不认识的选项，getopt 已经报过错
        }
    }

    // 把剩下的参数收集成操作数
    struct operand_list list = build_operand_list(argv, argc, optind, &opt);
    if (list.items == NULL)
    {
        perror("calloc");
        return 1;
    }

    // 多个操作数或者 -R 时，目录要打印 "路径:" 这样的标题
    int header = (list.total > 1 || opt.recursive);
    int printed = 0; // 之前有没有打印过东西，用来控制空行
    int status = (list.errors > 0) ? 2 : 0; // 有操作数打不开，退出码至少是 2

    // 操作数本身也要排序
    if (opt.sort == SORT_TIME)
        qsort(list.items, list.count, sizeof(struct file_info), compare_mtime);
    else
        qsort(list.items, list.count, sizeof(struct file_info), compare_name);

    // -r：排好序之后整体倒过来
    if (opt.reverse)
        reverse_files(list.items, list.count);

    // 文件操作数先打印，它们没有标题，也没有 total
    for (int i = 0; i < list.count; i++)
    {
        if (!S_ISDIR(list.items[i].st.st_mode))
        {
            do_print(&list.items[i], 1, &opt, NULL);
            printed = 1;
        }
    }

    // 目录操作数后打印
    for (int i = 0; i < list.count; i++)
    {
        if (S_ISDIR(list.items[i].st.st_mode))
        {
            // 标题由 list_dir 在目录打开成功后打印；
            // printed 控制“第二个及以后的目录前面空一行”
            int rc = list_dir(list.items[i].name, 0, &opt, header, printed);
            if (rc > status)
                status = rc; // 记录错误码

            printed = 1;
        }
    }

    free(list.items); // 操作数数组用完了
    return status;    // 0 表示正常，1 表示子目录有问题，2 表示命令行参数有问题
}
