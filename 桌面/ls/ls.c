#include <stdio.h>    
#include <stdlib.h>  
#include <dirent.h>   
#include <string.h>   
#include <sys/stat.h>  
#include <unistd.h>    
#include <time.h>      
#include <errno.h>     
#include <pwd.h>          
#include <grp.h>          
#include <limits.h>       
#include <sys/sysmacros.h> 

// 半年大约是多少秒。ls 用它判断一个文件是不是“老文件”
#define SIX_MONTHS (31556952 / 2)  
enum sort_key { SORT_NAME, SORT_TIME };  // SORT_NAME 是默认，SORT_TIME 对应 -t

// 所有命令行开关都放在这个结构体里，函数之间只传一个指针
struct ls_options {  // 命令行选项集合开始
    int all;          // -a：显示隐藏文件
    int inode;        // -i：显示 inode 号
    int long_fmt;     // -l：显示详细信息
    int blocks;       // -s：显示占用块数
    int recursive;    // -R：递归列出子目录
    int reverse;      // -r：反向排序
    enum sort_key sort; // -t：按时间排序；默认按名字
};  
struct file_info { 
    char *name;       // 命令行上的路径
    struct stat st;   // stat 拿到的详细信息
}; 

// 命令行操作数的收集
struct operand_list { 
    struct file_info *items; // 存所有操作数的数组，调用方负责 free
    int count;               // 成功 lstat 的条数
    int total;               // 命令行上一共给了几个操作数
    int errors;              // lstat 失败的个数
};  

// 把用户ID变成用户名。查不到就显示数字
static void uid_to_name(uid_t uid, char *buf, size_t size) {  // 输入用户ID，结果写进 buf
    struct passwd *pw = getpwuid(uid);   // 去系统用户表里查
    if (pw != NULL)  // 查到了这个用户
        snprintf(buf, size, "%s", pw->pw_name); // 查到了，复制用户名
    else  // 用户表里没有这个ID
        snprintf(buf, size, "%u", (unsigned)uid); // 查不到，显示数字
}  // uid_to_name 结束

// 把组ID变成组名。查不到就显示数字
static void gid_to_name(gid_t gid, char *buf, size_t size) {  // 输入组ID，结果写进 buf
    struct group *gr = getgrgid(gid);    // 去系统组表里查
    if (gr != NULL)  // 查到了这个组
        snprintf(buf, size, "%s", gr->gr_name); // 查到了，复制组名
    else  // 组表里没有这个ID
        snprintf(buf, size, "%u", (unsigned)gid); // 查不到，显示数字
} 

// 把“文件大小”变成字符串
// 普通文件显示字节数；设备文件显示“主设备号, 次设备号”
static void size_to_str(const struct stat *st, char *buf, size_t size) {  // st 是文件信息，结果写进 buf
    if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))  // 字符设备或块设备
        snprintf(buf, size, "%u, %u", major(st->st_rdev), minor(st->st_rdev));  // 设备号格式
    else  // 其他类型的文件
        snprintf(buf, size, "%ld", (long)st->st_size);  // 普通文件显示字节数
}  

// 每一列要占多宽
struct col_widths {  // 存放六列的最大宽度
    int inode;   // inode 列宽度
    int blocks;  // 块数列宽度
    int nlink;   // 硬链接数列宽度
    int owner;   // 用户名列宽度
    int group;   // 组名列宽度
    int size;    // 大小列宽度
}; 

// 遍历所有条目，算出每一列的最大宽度
static struct col_widths compute_widths(const struct file_info *files, int count,  // 条目数组和条目数
                                        const struct ls_options *opt) {  // 命令行选项
    struct col_widths w = {0};   
    char buf[256];               // 临时存放格式化后的文字
    for (int i = 0; i < count; i++) {
        const struct stat *st = &files[i].st;  // 指向当前条目的 stat
        int n; // snprintf 的返回值，正好就是字符串长度
        if (opt->inode) {
            n = snprintf(buf, sizeof buf, "%lu", (unsigned long)st->st_ino);  // 格式化 inode
            if (n > w.inode) w.inode = n; // 记录最大宽度
        }  
        if (opt->blocks) {
            n = snprintf(buf, sizeof buf, "%lu",  // 格式化块数
                         (unsigned long)((st->st_blocks + 1) / 2));  // 512B 单位换算成 1K
            if (n > w.blocks) w.blocks = n;  // 记录最大宽度
        }  
        n = snprintf(buf, sizeof buf, "%lu", (unsigned long)st->st_nlink);  // 格式化硬链接数
        if (n > w.nlink) w.nlink = n; // 硬链接数最大几位
        uid_to_name(st->st_uid, buf, sizeof buf);  // 查用户名
        n = (int)strlen(buf);  // 用户名长度
        if (n > w.owner) w.owner = n; // 用户名最长几个字符
        gid_to_name(st->st_gid, buf, sizeof buf);  // 查组名
        n = (int)strlen(buf);  // 组名长度
        if (n > w.group) w.group = n; // 组名最长几个字符
        size_to_str(st, buf, sizeof buf);  // 格式化大小
        n = (int)strlen(buf);  // 大小字符串长度
        if (n > w.size) w.size = n;   // 大小最长几个字符
    }  
    return w; 
}  

// 把权限位变成 "drwxr-xr-x" 这样的字符串
void mode_to_str(mode_t mode, char *buf) {  // 输入 st_mode，输出 11 字节字符串
    // 第 0 位：文件类型
    if (S_ISDIR(mode))  // 是目录吗
        buf[0] = 'd';            // 目录
    else if (S_ISLNK(mode))  // 是符号链接吗
        buf[0] = 'l';            // 符号链接
    else if (S_ISCHR(mode))  // 是字符设备吗
        buf[0] = 'c';            // 字符设备
    else if (S_ISBLK(mode))  // 是块设备吗
        buf[0] = 'b';            // 块设备
    else if (S_ISFIFO(mode))  // 是管道文件吗
        buf[0] = 'p';            // 管道文件
    else if (S_ISSOCK(mode))  // 是 socket 文件吗
        buf[0] = 's';            // socket 文件
    else  
        buf[0] = '-';            // 普通文件
    // 第 1~9 位：用户、组、其他人的 rwx
    const char *perms = "rwxrwxrwx";  // 九种权限对应的字母
    for (int i = 0; i < 9; i++) {  // 逐个权限位处理
        if (mode & (1 << (8 - i))) // 这一位是 1，说明有这个权限
            buf[1 + i] = perms[i];  // 填对应的字母
        else  // 这一位是 0
            buf[1 + i] = '-';      // 没有这个权限
    } 
    // 有执行位显示小写 s/t，没有执行位显示大写 S/T
    if (mode & S_ISUID)  // setuid 位
        buf[3] = (mode & S_IXUSR) ? 's' : 'S';  // 属主执行位为 1 用小写 s
    if (mode & S_ISGID)  // setgid 位
        buf[6] = (mode & S_IXGRP) ? 's' : 'S';  // 组执行位为 1 用小写 s
    if (mode & S_ISVTX)  // sticky 位
        buf[9] = (mode & S_IXOTH) ? 't' : 'T';  // 其他人执行位为 1 用小写 t
    buf[10] = '\0'; // 字符串结尾
} 

// ==== 颜色（ANSI 转义序列） ====
//
// 终端颜色用 ANSI 转义序列表示，格式是：ESC [ 参数 m
// ESC 写成 \033（八进制），参数用分号分隔：
//   01 表示加粗，34 表示蓝色，36 表示青色，32 表示绿色，31 表示红色……
#define COLOR_RESET "\033[0m"  // 复位码

// 按文件类型返回对应的颜色转义序列
// 普通文件返回空串 ""，表示不需要上色
static const char *color_for_mode(mode_t mode) {  // 输入 st_mode，返回颜色字符串
    if (S_ISDIR(mode))  // 是目录吗
        return "\033[01;34m";              // 目录：亮蓝
    if (S_ISLNK(mode))  // 是符号链接吗
        return "\033[01;36m";              // 符号链接：亮青
    if (S_ISFIFO(mode))  // 是命名管道吗
        return "\033[33m";                 // 命名管道：黄
    if (S_ISSOCK(mode))  // 是套接字吗
        return "\033[01;35m";              // 套接字：红
    if (S_ISBLK(mode) || S_ISCHR(mode))  // 是块设备或字符设备吗
        return "\033[01;33m";              // 设备文件：黄
    if (S_ISREG(mode) && (mode & 0111))  // 是普通文件且有执行位吗
        return "\033[01;32m";              // 可执行文件：绿
    return "";                             // 普通文件：默认颜色
}  

// 按名字排序（正序）
int compare_name(const void *a, const void *b) {  // 
    struct file_info *filesA = (struct file_info *)a; 
    struct file_info *filesB = (struct file_info *)b;  
    return strcmp(filesA->name, filesB->name); // 名字小的排前面
} 

// 按修改时间排序：新的在前，旧的在后
int compare_mtime(const void *a, const void *b) {
    struct file_info *filesA = (struct file_info *)a;  
    struct file_info *filesB = (struct file_info *)b; 
    // 先比秒，再比纳秒
    if (filesA->st.st_mtim.tv_sec < filesB->st.st_mtim.tv_sec)  // A 更小
        return 1;  // A 更旧，A 往后放
    if (filesA->st.st_mtim.tv_sec > filesB->st.st_mtim.tv_sec)  // A 更大
        return -1; // A 更新，A 往前放
    if (filesA->st.st_mtim.tv_nsec < filesB->st.st_mtim.tv_nsec)  // 秒相同，比纳秒
        return 1;  // A 更旧，往后放
    if (filesA->st.st_mtim.tv_nsec > filesB->st.st_mtim.tv_nsec)  // A 更大
        return -1; // A 更新，往前放
    return 0; 
}  

// 把整个数组倒过来
// -r 是“排好序之后整体倒过来”，
static void reverse_files(struct file_info *files, int count) {
    for (int i = 0; i < count / 2; i++) {  // 只需交换前一半
        struct file_info temp = files[i];  
        files[i] = files[count - 1 - i];  
        files[count - 1 - i] = temp; 
    } 
}  

// 打印文件信息
// base 是“这些条目所在的目录路径”。目录里的条目需要它来拼完整路径；
void do_print(struct file_info *files, int count, const struct ls_options *opt,  
              const char *base) {
    struct col_widths w = compute_widths(files, count, opt); // 先量列宽
    time_t now = time(NULL); // 当前时间只取一次，所有文件共用
    // isatty 判断标准输出是不是终端：
    int use_color = isatty(STDOUT_FILENO);  // 1=终端，可以上色；0=管道/文件
    for (int i = 0; i < count; i++) {
        struct stat *st = &files[i].st;  // 指向当前条目的 stat
        if (opt->inode) {
            printf("%*lu ", w.inode, (unsigned long)st->st_ino); 
        }  
        if (opt->blocks) {
            printf("%*lu ", w.blocks,  
                (unsigned long)((st->st_blocks + 1) / 2)); 
        } 
        if (opt->long_fmt) {
            char mode_buf[11];                 // 权限字符串
            mode_to_str(st->st_mode, mode_buf);  // 把 st_mode 转成 drwxr-xr-x
            // 下面三个缓冲区分别存用户名、组名、大小
            char owner[256], group[256], size_buf[256]; 
            uid_to_name(st->st_uid, owner, sizeof owner);   
            gid_to_name(st->st_gid, group, sizeof group);  
            size_to_str(st, size_buf, sizeof size_buf);     
            // 时间
            char time_buf[64];  
            struct tm *tm_info = localtime(&st->st_mtime);  
            if (st->st_mtime > now + 3600 || now - st->st_mtime > SIX_MONTHS)  
                strftime(time_buf, sizeof time_buf, "%b %e  %Y", tm_info);  
            else  
                strftime(time_buf, sizeof time_buf, "%b %e %H:%M", tm_info);  
            // 按列宽打印
            printf("%s %*lu %-*s %-*s %*s %s ", 
                   mode_buf,  
                   w.nlink, (unsigned long)st->st_nlink,  
                   w.owner, owner,  
                   w.group, group,  
                   w.size, size_buf, 
                   time_buf);  
        }  
        const char *color = use_color ? color_for_mode(st->st_mode) : "";  // 取颜色或空串
        if (color[0] != '\0')  // 有颜色
            printf("%s%s%s", color, files[i].name, COLOR_RESET);
        else  
            printf("%s", files[i].name);  
        if (opt->long_fmt && S_ISLNK(st->st_mode)) {
            char link_path[PATH_MAX];  
            if (base != NULL)
                snprintf(link_path, sizeof link_path, "%s/%s", base, files[i].name); 
            else              
                snprintf(link_path, sizeof link_path, "%s", files[i].name);  
            char target[PATH_MAX]; 
            ssize_t n = readlink(link_path, target, sizeof target - 1); 
            if (n >= 0) {
                target[n] = '\0'; 
                const char *target_color = ""; 
                if (use_color) {
                    struct stat target_st;  
                    if (stat(link_path, &target_st) == 0)  
                        target_color = color_for_mode(target_st.st_mode);  
                    else  
                        target_color = "\033[01;31m"; 
                }  
                if (target_color[0] != '\0') 
                    printf(" -> %s%s%s", target_color, target, COLOR_RESET);  
                else  
                    printf(" -> %s", target);  
            }  
        }  
        printf("\n");
    } 
}  

// 列出 path 这个目录里的内容
int list_dir(const char *path, int depth, const struct ls_options *opt, 
             int show_header, int blank_before) {
    DIR *dp = opendir(path);
    if (dp == NULL) {
        fprintf(stderr, "%s: cannot open directory '%s': %s\n", 
                "ls", path, strerror(errno));  
        return (depth == 0) ? 2 : 1;  
    } 
    if (show_header) {
        if (blank_before)
            printf("\n");  
        printf("%s:\n", path); 
    }  
    int status = 0;          
    struct file_info *files; 
    int count = 0;         
    int capacity = 64;       
    struct dirent *entry;    
    files = malloc(capacity * sizeof(struct file_info)); 
    if (files == NULL) {
        perror("malloc");  
        closedir(dp);  
        return 1; 
    } 
    while ((entry = readdir(dp)) != NULL) {
        if (opt->all == 0 && entry->d_name[0] == '.')  
            continue;  
        if (count == capacity) {
            capacity *= 2; 
            struct file_info *temp = realloc(files, capacity * sizeof(struct file_info)); 
            if (temp == NULL) {
                perror("realloc");  
                closedir(dp); 
                return 1;  
            } 
            files = temp;  
        } 
        files[count].name = strdup(entry->d_name); 
        if (files[count].name == NULL) {
            perror("strdup"); 
            status = 1; 
            break;  
        }  
        char full_path[PATH_MAX]; 
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name); 
        if (lstat(full_path, &files[count].st) != 0) {
            free(files[count].name); 
            continue;  
        }  
        count++;  
    }  
    closedir(dp); 
    // 排序
    if (opt->sort == SORT_TIME) 
        qsort(files, count, sizeof(struct file_info), compare_mtime);  // 时间比较函数
    else  // 按名字排序
        qsort(files, count, sizeof(struct file_info), compare_name);  // 名字比较函数
    if (opt->reverse)  
        reverse_files(files, count); 
    if (opt->blocks || opt->long_fmt) {
        unsigned long total = 0; 
        for (int i = 0; i < count; i++) {
            total += (unsigned long)files[i].st.st_blocks;
        }  
        printf("total %lu\n", (total + 1) / 2); 
    } 
    do_print(files, count, opt, path); 
    for (int i = 0; i < count; i++) {
        if (opt->recursive) {
            if (S_ISDIR(files[i].st.st_mode) &&  
                (strcmp(files[i].name, ".") != 0) &&  
                (strcmp(files[i].name, "..") != 0)) {
                char full_path[PATH_MAX];  
                snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i].name);  
                int rc = list_dir(full_path, depth + 1, opt, 1, 1);  
                if (rc > status)
                    status = rc; 
            }  
        }  
        free(files[i].name);
    } 
    free(files); 
    return status;
} 
static struct operand_list build_operand_list(char **argv, int argc, int optind, 
                                              const struct ls_options *opt) {
    struct operand_list list = {0}; 
    int noperands = argc - optind;  
    list.total = (noperands > 0) ? noperands : 1;  
    list.items = calloc(list.total, sizeof(struct file_info));
    if (list.items == NULL)  
        return list; 
    for (int i = 0; i < list.total; i++) {
        const char *path = (noperands > 0) ? argv[optind + i] : "."; 
        struct stat st_entry, st_target; 
        if (lstat(path, &st_entry) != 0) {
            fprintf(stderr, "%s: cannot access '%s': %s\n",  
                    "ls", path, strerror(errno));  
            list.errors++;  
            continue; 
        }  
        int list_contents = S_ISDIR(st_entry.st_mode);  
        if (!list_contents && S_ISLNK(st_entry.st_mode) && !opt->long_fmt) {
            if (stat(path, &st_target) == 0 && S_ISDIR(st_target.st_mode))  
                list_contents = 1;  
        }  
        list.items[list.count].name = (char *)path; 
        list.items[list.count].st =  
            (list_contents && S_ISLNK(st_entry.st_mode)) ? st_target : st_entry;  
        list.count++;  
    } 
    return list;  
}  
int main(int argc, char *argv[]) {
    struct ls_options opt = {0}; 
    int c; 
    while ((c = getopt(argc, argv, "ailRrst")) != -1) {
        switch (c) {
            case 'a': opt.all = 1; break;          // -a
            case 'i': opt.inode = 1; break;        // -i
            case 'l': opt.long_fmt = 1; break;     // -l
            case 'R': opt.recursive = 1; break;    // -R
            case 'r': opt.reverse = 1; break;      // -r
            case 's': opt.blocks = 1; break;       // -s
            case 't': opt.sort = SORT_TIME; break; // -t
            default: 
                return 2; 
        }  
    }  
    struct operand_list list = build_operand_list(argv, argc, optind, &opt);  
    if (list.items == NULL) {
        perror("calloc");  
        return 1;  
    }  
    int header = (list.total > 1 || opt.recursive); 
    int printed = 0; 
    int status = (list.errors > 0) ? 2 : 0; 
    if (opt.sort == SORT_TIME)  
        qsort(list.items, list.count, sizeof(struct file_info), compare_mtime);  
    else 
        qsort(list.items, list.count, sizeof(struct file_info), compare_name);  
    if (opt.reverse)  
        reverse_files(list.items, list.count);  
    for (int i = 0; i < list.count; i++) {
        if (!S_ISDIR(list.items[i].st.st_mode)) {
            do_print(&list.items[i], 1, &opt, NULL); 
            printed = 1;  
        } 
    }  
    for (int i = 0; i < list.count; i++) {
        if (S_ISDIR(list.items[i].st.st_mode)) {
            int rc = list_dir(list.items[i].name, 0, &opt, header, printed); 
            if (rc > status)  
                status = rc; 
            printed = 1;  
        }  
    }  
    free(list.items); 
    return status;    // 0 表示正常，1 表示子目录有问题，2 表示命令行参数有问题
}  
