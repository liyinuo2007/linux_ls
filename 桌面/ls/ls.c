#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

struct file_info
{
    char *name;
    ino_t inode;
    struct stat st;
};

int compare_name(const void *a, const void *b)
{
    struct file_info *filesA = (struct file_info *)a;
    struct file_info *filesB = (struct file_info *)b;
    // 比较两个字符串，如果A比B小，返回负数，A在前
    return strcmp(filesA->name, filesB->name);
}

int compare_name_reverse(const void *a, const void *b)
{
    struct file_info *filesA = (struct file_info *)a;
    struct file_info *filesB = (struct file_info *)b;
    // 比较两个字符串，如果A比B小，返回负数，A在前
    return -strcmp(filesA->name, filesB->name);
}

int compare_mtime(const void *a, const void *b)
{
    struct file_info *filesA = (struct file_info *)a;
    struct file_info *filesB = (struct file_info *)b;

    // A旧A往后
    if (filesA->st.st_mtime < filesB->st.st_mtime)
    {
        return 1;
    }

    // A新A往前
    if (filesA->st.st_mtime > filesB->st.st_mtime)
    {
        return -1;
    }

    // 时间相同：名字逆序（GNU 实测行为）
    return -strcmp(filesA->name, filesB->name);
}

int compare_mtime_reverse(const void *a, const void *b)
{
    struct file_info *filesA = (struct file_info *)a;
    struct file_info *filesB = (struct file_info *)b;
    // a 旧 → a 往前
    if (filesA->st.st_mtime < filesB->st.st_mtime)
        return -1;
    // a 新 → a 往后
    if (filesA->st.st_mtime > filesB->st.st_mtime)
        return 1;
    // 并列：名字正序
    return strcmp(filesA->name, filesB->name);
}

void mode_to_str(mode_t mode, char *buf)
{
    // buf 至少 11 字节
    // 第 0 位：类型字符
    if (S_ISDIR(mode))
        buf[0] = 'd';
    else if (S_ISLNK(mode))
        buf[0] = 'l';
    else
        buf[0] = '-';

    // 第 1~9 位：9 个权限位，从高到低
    const char *perms = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
    {
        if (mode & (1 << (8 - i)))
            buf[1 + i] = perms[i];
        else
            buf[1 + i] = '-';
    }

    buf[10] = '\0';
}

void do_print(struct file_info *files, int count, int has_i, int has_l,int has_s)
{
    for (int i = 0; i < count; i++)
    {
        if (has_i)
        {
            printf("%-8lu ", (unsigned long)files[i].inode);
        }

        if(has_s)
        {
            printf("%-5lu ",(unsigned long)((files[i].st.st_blocks+1)/2));
        }
        
        if (has_l)
        {
            char mode_buf[11];
            mode_to_str(files[i].st.st_mode, mode_buf);
            printf("%s %lu %-5u %-5u %-8ld ",mode_buf,(unsigned long)files[i].st.st_nlink,(unsigned int)files[i].st.st_uid,(unsigned int)files[i].st.st_gid,(long)files[i].st.st_size);
            struct tm *tm_info = localtime(&files[i].st.st_mtime);
            char time_buf[64];
            // 将原来的时间换算成年月日
            strftime(time_buf, sizeof(time_buf), "%b %e %H:%M", tm_info);
            printf("%s ", time_buf);
        }

        printf("%s\n", files[i].name);
    }
}

void list_dir(const char *path, int depth, int has_a, int has_i, int has_l,int has_r, int has_R, int has_t,int has_s)
{
    DIR *dp = opendir(path);
    if (dp == NULL)
    {
        // 打印上一次调用失败的具体原因
        perror("opendir");
        return;
    }

    if (has_R)
    {
        if (depth > 0)
        {
            printf("\n");
        }
        printf("%s:\n", path);
    }

    // 存放readdir的文件名
    struct file_info *files;
    // names的存储空间序号
    int count = 0;
    // 当前最大容量
    int capacity = 64;
    // 接收readdir返回值
    struct dirent *entry;

    files = malloc(capacity * sizeof(struct file_info));
    if (files == NULL)
    {
        perror("malloc");
        closedir(dp);
        return;
    }

    while ((entry = readdir(dp)) != NULL)
    {
        if (has_a == 0 && entry->d_name[0] == '.')
            continue;
        // 此时空间刚好装满，再装会越界
        if (count == capacity)
        {
            // 下一次空间满了继续扩容
            capacity *= 2;
            // realloc先扩容，再将names里的文件名地址拷贝到新空间temp中，再释放掉原来的旧空间
            struct file_info *temp = realloc(files, capacity * sizeof(struct file_info));
            if (temp == NULL)
            {
                perror("realloc");
                return;
            }
            files = temp;
        }
        files[count].name = strdup(entry->d_name);
        files[count].inode = entry->d_ino;
        char full_path[1024];
        // 用于将格式化数据写入指定大小的缓冲区并防止溢出
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        lstat(full_path, &files[count].st);
        if (files[count].name == NULL)
        {
            perror("strdup");
            break;
        }
        count++;
    }
    closedir(dp);

    if (has_t)
    {
        if (has_r)
            qsort(files, count, sizeof(struct file_info), compare_mtime_reverse);
        else
            qsort(files, count, sizeof(struct file_info), compare_mtime);
    }
    else
    {
        if (has_r)
            qsort(files, count, sizeof(struct file_info), compare_name_reverse);
        else
            qsort(files, count, sizeof(struct file_info), compare_name);
    }

    if(has_s)
    {
        unsigned long total=0;
        for(int i=0;i<count;i++)
        {
            total+=(long)((files[i].st.st_blocks+1)/2);
        }
        printf("total %-5lu\n",total);
    }

    do_print(files, count, has_i, has_l,has_s);

    for (int i = 0; i < count; i++)
    {
        if (has_R)
        {
            // 判断是否有子目录，是否需要递归；跳过 . 和 .. 是为了防止无限递归 / 跑回父目录.
            if (S_ISDIR(files[i].st.st_mode) &&
                (strcmp(files[i].name, ".") != 0) &&
                (strcmp(files[i].name, "..") != 0))
            {
                char full_path[1024];
                // 路径拼接的作用：把“目录里的名字”翻译成“文件系统里的地址”
                snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i].name);
                list_dir(full_path, depth + 1, has_a, has_i, has_l, has_r, has_R, has_t,has_s);
            }
        }
        free(files[i].name);
    }

    free(files);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        // 判断传入的参数有没有执行的动作
        printf("用法：%s<路径><动作1><动作2>...\n", argv[0]);
        return 1;
    }

    // 以下皆为函数开关
    int has_a = 0;
    int has_i = 0;
    int has_l = 0;
    int has_R = 0;
    int has_r = 0;
    int has_s = 0;
    int has_t = 0;

    for (int i = 0; i < argc; i++)  //
    {
        // 是否过滤隐藏文件的开关
        if (strcmp(argv[i], "-a") == 0)
            has_a = 1;
        if (strcmp(argv[i], "-i") == 0)
            has_i = 1;
        if (strcmp(argv[i], "-l") == 0)
            has_l = 1;
        if (strcmp(argv[i], "-R") == 0)
            has_R = 1;
        if (strcmp(argv[i], "-r") == 0)
            has_r = 1;
        if (strcmp(argv[i], "-s") == 0)
            has_s = 1;
        if (strcmp(argv[i], "-t") == 0)
            has_t = 1;
    }

    int depth = 0;
    list_dir(argv[1], depth, has_a, has_i, has_l, has_r, has_R, has_t,has_s);
    return 0;
}
