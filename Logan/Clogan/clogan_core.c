/*
 * Copyright (c) 2018-present, 美团点评
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "clogan_core.h"

#include "mmap_util.h"
#include "construct_data.h"
#include "cJSON.h"
#include "json_util.h"
#include "zlib_util.h"
#include "aes_util.h"
#include "directory_util.h"
#include "base_util.h"
#include "console_util.h"
#include "clogan_status.h"
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdatomic.h>
#include <android/log.h>

static int is_init_ok = 0;
static int is_open_ok = 0;

#define  LOGI(...) __android_log_print(ANDROID_LOG_INFO, "========= Info =========   ", __VA_ARGS__)

static unsigned char *_logan_buffer = NULL; //缓存Buffer (不释放)

static char *_dir_path = NULL; //目录路径 (不释放)

static char *_file_name = NULL; //目录文件 做前缀 (不释放)

static int _file_count = 0; //当日文件数量

static char *_mmap_file_path = NULL; //mmap文件路径 (不释放)

static int buffer_length = 0; //缓存区域的大小

static unsigned char *_cache_buffer_buffer = NULL; //临时缓存文件 (不释放)

static int buffer_type; //缓存区块的类型

static long max_file_len = LOGAN_LOGFILE_MAXLENGTH; //单个文件最大 文件大小

static long max_file_count = LOGAN_LOGFILE_MAXCOUNT; // 单日最大 文件数量

static cLogan_model *logan_model = NULL; //(不释放)

bool IsDigitChar(char i);

bool IsValidFullDateStr(char date[15]);

void get_date_before_days(int days, char date[15]);

int delete_file_by_day(char *fileDir, int days, int clearMode);

int getDayFileCount(char *fileDir);

char *getDayLastFileName(char *fileDir);

int init_file_clogan(cLogan_model *logan_model) {
    int is_ok = 0;
    if (LOGAN_FILE_OPEN == logan_model->file_stream_type) {
        return 1;
    } else {
        LOGI("init_file_clogan:[%s]",logan_model->file_path);
        FILE *file_temp = fopen(logan_model->file_path, "ab+");
        if (NULL != file_temp) {  //初始化文件流开启
            logan_model->file = file_temp;
            fseek(file_temp, 0, SEEK_END);
            long longBytes = ftell(file_temp);
            logan_model->file_len = longBytes;
            logan_model->file_stream_type = LOGAN_FILE_OPEN;
            is_ok = 1;
        } else {
            logan_model->file_stream_type = LOGAN_FILE_NONE;
        }
    }
    return is_ok;
}

void init_encrypt_key_clogan(cLogan_model *logan_model) {
    aes_inflate_iv_clogan(logan_model->aes_iv);
}

void write_mmap_data_clogan(char *path, unsigned char *temp) {
    logan_model->total_point = temp;
    logan_model->file_path = path;
    char len_array[] = {'\0', '\0', '\0', '\0'};
    len_array[0] = *temp;
    temp++;
    len_array[1] = *temp;
    temp++;
    len_array[2] = *temp;

    adjust_byteorder_clogan(len_array);//调整字节序,默认为低字节序,在读取的地方处理

    int *total_len = (int *) len_array;

    int t = *total_len;
    printf_clogan("write_mmapdata_clogan > buffer total length %d\n", t);
    if (t > LOGAN_WRITEPROTOCOL_HEAER_LENGTH && t < LOGAN_MMAP_LENGTH) {
        logan_model->total_len = t;
        if (NULL != logan_model) {
            if (init_file_clogan(logan_model)) {
                logan_model->is_ok = 1;
                logan_model->zlib_type = LOGAN_ZLIB_NONE;
                clogan_flush();
                fclose(logan_model->file);
                logan_model->file_stream_type = LOGAN_FILE_CLOSE;

            }
        }
    } else {
        logan_model->file_stream_type = LOGAN_FILE_NONE;
    }
    logan_model->total_len = 0;
    logan_model->file_path = NULL;
}

void read_mmap_data_clogan(const char *path_dirs) {
    if (buffer_type == LOGAN_MMAP_MMAP) {
        unsigned char *temp = _logan_buffer;
        unsigned char *temp2 = NULL;
        char i = *temp;
        if (LOGAN_MMAP_HEADER_PROTOCOL == i) {
            temp++;
            char len_array[] = {'\0', '\0', '\0', '\0'};
            len_array[0] = *temp;
            temp++;
            len_array[1] = *temp;
            adjust_byteorder_clogan(len_array);
            int *len_p = (int *) len_array;
            temp++;
            temp2 = temp;
            int len = *len_p;

            printf_clogan("read_mmapdata_clogan > path's json length : %d\n", len);

            if (len > 0 && len < 1024) {
                temp += len;
                i = *temp;
                if (LOGAN_MMAP_TAIL_PROTOCOL == i) {
                    char dir_json[len];
                    memset(dir_json, 0, len);
                    memcpy(dir_json, temp2, len);
                    printf_clogan("dir_json %s\n", dir_json);
                    cJSON *cjson = cJSON_Parse(dir_json);

                    if (NULL != cjson) {
                        cJSON *dir_str = cJSON_GetObjectItem(cjson,
                                                             LOGAN_VERSION_KEY);  //删除json根元素释放
                        cJSON *path_str = cJSON_GetObjectItem(cjson, LOGAN_PATH_KEY);
                        if ((NULL != dir_str && cJSON_Number == dir_str->type &&
                             CLOGAN_VERSION_NUMBER == dir_str->valuedouble) &&
                            (NULL != path_str && path_str->type == cJSON_String &&
                             !is_string_empty_clogan(path_str->valuestring))) {

                            printf_clogan(
                                    "read_mmapdata_clogan > dir , path and version : %s || %s || %lf\n",
                                    path_dirs, path_str->valuestring, dir_str->valuedouble);
                            LOGI("read_mmapdata_clogan > dir , path and version : %s || %s || %lf\n",
                                 path_dirs, path_str->valuestring, dir_str->valuedouble);

                            size_t dir_len = strlen(path_dirs);
                            size_t path_len = strlen(path_str->valuestring);
                            size_t length = dir_len + path_len + 1;
                            char file_path[length];
                            memset(file_path, 0, length);
                            memcpy(file_path, path_dirs, dir_len);
                            strcat(file_path, path_str->valuestring);
                            temp++;
                            write_mmap_data_clogan(file_path, temp);
                        }
                        cJSON_Delete(cjson);
                    }
                }
            }
        }
    }
}

/**
 * Logan初始化
 * @param cachedirs 缓存路径
 * @param pathdirs  目录路径
 * @param max_file  日志文件最大值
 */
int
clogan_init(const char *cache_dirs, const char *path_dirs, int max_file,int max_count,const char *encrypt_key16,
            const char *encrypt_iv16) {
    int back = CLOGAN_INIT_FAIL_HEADER;
    if (is_init_ok ||
        NULL == cache_dirs || strnlen(cache_dirs, 11) == 0 ||
        NULL == path_dirs || strnlen(path_dirs, 11) == 0 ||
        NULL == encrypt_key16 || NULL == encrypt_iv16) {
        back = CLOGAN_INIT_FAIL_HEADER;
        return back;
    }

    if (max_file > 0) {
        max_file_len = max_file;
    } else {
        max_file_len = LOGAN_LOGFILE_MAXLENGTH;
    }

    if (max_count > 0) {
        max_file_count = max_count;
    } else {
        max_file_count = LOGAN_LOGFILE_MAXCOUNT;
    }

    if (NULL != _dir_path) { // 初始化时 , _dir_path和_mmap_file_path是非空值,先释放,再NULL
        free(_dir_path);
        _dir_path = NULL;
    }
    if (NULL != _mmap_file_path) {
        free(_mmap_file_path);
        _mmap_file_path = NULL;
    }

    aes_init_key_iv(encrypt_key16, encrypt_iv16);

    size_t path1 = strlen(cache_dirs);
    size_t path2 = strlen(LOGAN_CACHE_DIR);
    size_t path3 = strlen(LOGAN_CACHE_FILE);
    size_t path4 = strlen(LOGAN_DIVIDE_SYMBOL);

    int isAddDivede = 0;
    char d = *(cache_dirs + path1 - 1);
    if (d != '/') {
        isAddDivede = 1;
    }

    size_t total = path1 + (isAddDivede ? path4 : 0) + path2 + path4 + path3 + 1;
    char *cache_path = malloc(total);
    if (NULL != cache_path) {
        _mmap_file_path = cache_path; //保持mmap文件路径,如果初始化失败,注意释放_mmap_file_path
    } else {
        is_init_ok = 0;
        printf_clogan("clogan_init > malloc memory fail for mmap_file_path \n");
        back = CLOGAN_INIT_FAIL_NOMALLOC;
        return back;
    }

    memset(cache_path, 0, total);
    strcpy(cache_path, cache_dirs);
    if (isAddDivede)
        strcat(cache_path, LOGAN_DIVIDE_SYMBOL);

    strcat(cache_path, LOGAN_CACHE_DIR);
    strcat(cache_path, LOGAN_DIVIDE_SYMBOL);

    makedir_clogan(cache_path); //创建保存mmap文件的目录

    strcat(cache_path, LOGAN_CACHE_FILE);

    size_t dirLength = strlen(path_dirs);

    isAddDivede = 0;
    d = *(path_dirs + dirLength - 1);
    if (d != '/') {
        isAddDivede = 1;
    }
    total = dirLength + (isAddDivede ? path4 : 0) + 1;

    char *dirs = (char *) malloc(total); //缓存文件目录

    if (NULL != dirs) {
        _dir_path = dirs; //日志写入的文件目录
    } else {
        is_init_ok = 0;
        printf_clogan("clogan_init > malloc memory fail for _dir_path \n");
        back = CLOGAN_INIT_FAIL_NOMALLOC;
        return back;
    }
    memset(dirs, 0, total);
    memcpy(dirs, path_dirs, dirLength);
    if (isAddDivede)
        strcat(dirs, LOGAN_DIVIDE_SYMBOL);
    makedir_clogan(_dir_path); //创建缓存目录,如果初始化失败,注意释放_dir_path

    int flag = LOGAN_MMAP_FAIL;
    if (NULL == _logan_buffer) {
        if (NULL == _cache_buffer_buffer) {
            flag = open_mmap_file_clogan(cache_path, &_logan_buffer, &_cache_buffer_buffer);
        } else {
            flag = LOGAN_MMAP_MEMORY;
        }
    } else {
        flag = LOGAN_MMAP_MMAP;
    }

    if (flag == LOGAN_MMAP_MMAP) {
        buffer_length = LOGAN_MMAP_LENGTH;
        buffer_type = LOGAN_MMAP_MMAP;
        is_init_ok = 1;
        back = CLOGAN_INIT_SUCCESS_MMAP;
    } else if (flag == LOGAN_MMAP_MEMORY) {
        buffer_length = LOGAN_MEMORY_LENGTH;
        buffer_type = LOGAN_MMAP_MEMORY;
        is_init_ok = 1;
        back = CLOGAN_INIT_SUCCESS_MEMORY;
    } else if (flag == LOGAN_MMAP_FAIL) {
        is_init_ok = 0;
        back = CLOGAN_INIT_FAIL_NOCACHE;
    }

    if (is_init_ok) {
        if (NULL == logan_model) {
            logan_model = malloc(sizeof(cLogan_model));
            if (NULL != logan_model) { //堆非空判断 , 如果为null , 就失败
                memset(logan_model, 0, sizeof(cLogan_model));
            } else {
                is_init_ok = 0;
                printf_clogan("clogan_init > malloc memory fail for logan_model\n");
                back = CLOGAN_INIT_FAIL_NOMALLOC;
                return back;
            }
        }
        if (flag == LOGAN_MMAP_MMAP) //MMAP的缓存模式,从缓存的MMAP中读取数据
            read_mmap_data_clogan(_dir_path);
        printf_clogan("clogan_init > logan init success\n");
    } else {
        printf_clogan("clogan_open > logan init fail\n");
        // 初始化失败，删除所有路径
        if (NULL != _dir_path) {
            free(_dir_path);
            _dir_path = NULL;
        }
        if (NULL != _mmap_file_path) {
            free(_mmap_file_path);
            _mmap_file_path = NULL;
        }
    }
    delete_file_by_day(_dir_path, 3, 2);
    return back;
}


/**
 * 删除过期文件
 * fileDir:文件路径
 * days:天数
 * clearMode： 1、按照文件，2、按照文件创建日期
 */
int delete_file_by_day(char *fileDir, int days, int clearMode) {
    int ret = 0;
    int fileTotalNum = 0;
    int j = 0, len = 0, i = 0;
    char fullFileName[512 + 1];
    char fileName[256 + 1];
    char fileDate[14 + 1];
    char oldDate[14 + 1];

    struct dirent **nameList;
    struct stat fileInfo;
    struct tm *fileTM;
    // 获取前N天的日期
    memset(oldDate, 0, sizeof(oldDate));
    get_date_before_days(days, oldDate);
    // 打开目录
    if ((fileTotalNum = scandir(fileDir, &nameList, 0, alphasort)) < 0) {
        printf("in DeleteFileByDays:: scandir [%s] error!\n", fileDir);
        //free(nameList);
        printf("打开文件目录失败");
        return (fileTotalNum);
    }
    for (j = 0; j < fileTotalNum; j++) {
        len = sprintf(fileName, "%s", nameList[j]->d_name);
        if (fileName[0] == '.')
            continue;
        fileName[len] = 0;
        len = sprintf(fullFileName, "%s/%s", fileDir, fileName);
        fullFileName[len] = 0;
        if (clearMode == 1)     // 根据文件名清理
        {
            len = 0;
            for (i = 0; i < strlen(fileName); i++) {
                if (IsDigitChar(fileName[i]))
                    len += sprintf(fileDate + len, "%c", fileName[i]);
                else
                    len = 0;
                if (len == 8)
                    break;
            }
            fileDate[len] = 0;
            if (!IsValidFullDateStr(fileDate))
                continue;
        } else if (clearMode == 2)        // 根据文件属性
        {
            memset(&fileInfo, 0, sizeof(fileInfo));
            if ((ret = stat(fullFileName, &fileInfo)) < 0) {
                printf("in DeleteFileByDays:: stat [%s] error!\n", fullFileName);
                continue;
            }
            // 获取文件时间
            fileTM = localtime(&fileInfo.st_mtime);
            memset(fileDate, 0, sizeof(fileDate));
            strftime(fileDate, sizeof(fileDate), "%Y%m%d", fileTM);
        }
        // 检查是否需要删除文件
        if (strcmp(fileDate, oldDate) <= 0) {
            if ((ret = unlink(fullFileName)) < 0) {
                printf("in DeleteFileByDays:: unlink[%s] error!\n", fullFileName);
                continue;
            } else
                printf("in DeleteFileByDays:: delete[%s]!\n", fullFileName);
        }
    }
    free(nameList);
    return 0;
}

/**
 * 获取当日的文件数量
 */
int getDayFileCount(char *fileDir) {
    int ret = 0;
    int fileTotalNum = 0;
    int j = 0, len = 0, i = 0;
    char fullFileName[512 + 1];
    char fileName[256 + 1];
    char fileDate[14 + 1];
    char oldDate[14 + 1];

    struct dirent **nameList;
    struct stat fileInfo;
    struct tm *fileTM;
    // 获取前N天的日期 --- 获取当天N=0
    memset(oldDate, 0, sizeof(oldDate));
    get_date_before_days(0, oldDate);

    int fileCount = 0;
    // 打开目录
    if ((fileTotalNum = scandir(fileDir, &nameList, 0, alphasort)) < 0) {
        printf("in getDayFileCount:: scandir [%s] error!\n", fileDir);
        //free(nameList);
        printf("打开文件目录失败");
        return 0;
    }
    for (j = 0; j < fileTotalNum; j++) {
        len = sprintf(fileName, "%s", nameList[j]->d_name);
        if (fileName[0] == '.')
            continue;
        fileName[len] = 0;
        len = sprintf(fullFileName, "%s/%s", fileDir, fileName);
        fullFileName[len] = 0;
        {
            memset(&fileInfo, 0, sizeof(fileInfo));
            if ((ret = stat(fullFileName, &fileInfo)) < 0) {
                printf("in getDayFileCount:: stat [%s] error!\n", fullFileName);
                continue;
            }
            // 获取文件时间
            fileTM = localtime(&fileInfo.st_mtime);
            memset(fileDate, 0, sizeof(fileDate));
            strftime(fileDate, sizeof(fileDate), "%Y%m%d", fileTM);
        }
        // 检查是否需要删除文件
        if (strcmp(fileDate, oldDate) == 0) {
            fileCount++;
        }
    }
    free(nameList);
    return fileCount;
}

/**
 * 删除当日最早创建的文件
 */
int delete(char *fileDir) {
    int ret = 0;
    int fileTotalNum = 0;
    int j = 0, len = 0, i = 0;
    char fullFileName[512 + 1];
    char fileName[256 + 1];
    char fileDate[14 + 1];
    char fileDateINS[14 + 1];
    char oldDate[14 + 1];

    struct dirent **nameList;
    struct stat fileInfo;
    struct tm *fileTM;
    // 获取前N天的日期 --- 获取当天N=0
    memset(oldDate, 0, sizeof(oldDate));
    get_date_before_days(0, oldDate);

    int fileCount = 0;
    // 打开目录
    if ((fileTotalNum = scandir(fileDir, &nameList, 0, alphasort)) < 0) {
        printf("in getDayFileCount:: scandir [%s] error!\n", fileDir);
        //free(nameList);
        printf("打开文件目录失败");
        return 0;
    }
    char *deleteFileData = NULL;
    char deleteFileName[512 + 1];

    for (j = 0; j < fileTotalNum; j++) {
        len = sprintf(fileName, "%s", nameList[j]->d_name);
        if (fileName[0] == '.')
            continue;
        fileName[len] = 0;
        len = sprintf(fullFileName, "%s/%s", fileDir, fileName);
        fullFileName[len] = 0;

        memset(&fileInfo, 0, sizeof(fileInfo));
        if ((ret = stat(fullFileName, &fileInfo)) < 0) {
            printf("in getDayFileCount:: stat [%s] error!\n", fullFileName);
            continue;
        }
        // 获取文件时间
        fileTM = localtime(&fileInfo.st_mtime);
        memset(fileDate, 0, sizeof(fileDate));
        strftime(fileDate, sizeof(fileDate), "%Y%m%d", fileTM);
        memset(fileDateINS, 0, sizeof(fileDateINS));
        strftime(fileDateINS, sizeof(fileDateINS), "%Y%m%d%H%M%S", fileTM);
        // 检查是否需要删除文件
        if (strcmp(fileDate, oldDate) == 0) {
            if (deleteFileData == NULL) {
                deleteFileData = (char *) malloc(strlen(fileDateINS) + 1);
                strcpy(deleteFileData, fileDateINS);
                strcpy(deleteFileName, fullFileName);
                continue;
            }
            if (strcmp(deleteFileData, fileDateINS) > 0) {
                strcpy(deleteFileData, fileDateINS);
                strcpy(deleteFileName, fullFileName);
            }
        }
    }
    free(deleteFileData);
    deleteFileData = NULL;
    if ((ret = unlink(deleteFileName)) >= 0) {
        LOGI("delete file name [%s]", deleteFileName);
    }
    free(nameList);
    return deleteFileName;
}

/**
 * 获取当日最后创建的文件
 */
char *getDayLastFileName(char *fileDir) {
    int ret = 0;
    int fileTotalNum = 0;
    int j = 0, len = 0, i = 0;
    char fullFileName[512 + 1];
    char fileName[256 + 1];
    char fileDate[14 + 1];
    char fileDateINS[14 + 1];
    char oldDate[14 + 1];

    struct dirent **nameList;
    struct stat fileInfo;
    struct tm *fileTM;
    // 获取前N天的日期 --- 获取当天N=0
    memset(oldDate, 0, sizeof(oldDate));
    get_date_before_days(0, oldDate);
    LOGI("getDayLastFileName....");
    int fileCount = 0;
    // 打开目录
    if ((fileTotalNum = scandir(fileDir, &nameList, 0, alphasort)) < 0) {
        printf("in getDayFileCount:: scandir [%s] error!\n", fileDir);
        //free(nameList);
        printf("打开文件目录失败");
        return 0;
    }
    char *deleteFileData = NULL;
    char *deleteFileName = NULL;

    for (j = 0; j < fileTotalNum; j++) {
        len = sprintf(fileName, "%s", nameList[j]->d_name);
        if (fileName[0] == '.')
            continue;
        fileName[len] = 0;
        len = sprintf(fullFileName, "%s/%s", fileDir, fileName);
        fullFileName[len] = 0;

        memset(&fileInfo, 0, sizeof(fileInfo));
        if ((ret = stat(fullFileName, &fileInfo)) < 0) {
            printf("in getDayFileCount:: stat [%s] error!\n", fullFileName);
            continue;
        }
        // 获取文件时间
        fileTM = localtime(&fileInfo.st_mtime);
        memset(fileDate, 0, sizeof(fileDate));
        strftime(fileDate, sizeof(fileDate), "%Y%m%d", fileTM);
        memset(fileDateINS, 0, sizeof(fileDateINS));
        strftime(fileDateINS, sizeof(fileDateINS), "%Y%m%d%H%M%S", fileTM);
        if (strcmp(fileDate, oldDate) == 0) {
            LOGI("fileDateINS:[%s]",fileDateINS);
            if (deleteFileData == NULL) {
                deleteFileData = (char *) malloc(strlen(fileDateINS) + 1);
                deleteFileName = (char *) malloc(strlen(fullFileName) + 1);
                strcpy(deleteFileData, fileDateINS);
                strcpy(deleteFileName, fullFileName);
                continue;
            }
            if (strcmp(deleteFileData, fileDateINS) < 0) {
                strcpy(deleteFileData, fileDateINS);
                strcpy(deleteFileName, fullFileName);
            }
        }
    }
    free(deleteFileData);
    deleteFileData = NULL;
    LOGI("getDayLastFileName fullFileName [%s]", deleteFileName);
    free(nameList);
    return deleteFileName;
}


void get_date_before_days(int days, char date[15]) {
    time_t lt = time(NULL);
    long seconds = 24 * 60 * 60 * days;//  24小时*days
    lt -= seconds;
    struct tm *times = localtime(&lt);
    strftime(date, sizeof(date), "%Y%m%d", times);
}


void get_format_time_string(char *str_time) //获取格式化时间
{
    time_t now;
    struct tm *tm_now;
    char datetime[128];
    time(&now);
    tm_now = localtime(&now);
    strftime(datetime, 128, "%Y-%m-%d %H:%M:%S", tm_now);
    printf("now datetime : %s\n", datetime);
    strcpy(str_time, datetime);
}


bool IsValidFullDateStr(char date[15]) {
    return 0;
}

bool IsDigitChar(char i) {
    return 0;
}


/*
 * 对mmap添加header和确定总长度位置
 */
void add_mmap_header_clogan(char *content, cLogan_model *model) {
    size_t content_len = strlen(content) + 1;
    size_t total_len = content_len;
    char *temp = (char *) model->buffer_point;
    *temp = LOGAN_MMAP_HEADER_PROTOCOL;
    temp++;
    *temp = total_len;
    temp++;
    *temp = total_len >> 8;
    printf_clogan("\n add_mmap_header_clogan len %d\n", total_len);
    temp++;
    memcpy(temp, content, content_len);
    temp += content_len;
    *temp = LOGAN_MMAP_TAIL_PROTOCOL;
    temp++;
    model->total_point = (unsigned char *) temp; // 总数据的total_length的指针位置
    model->total_len = 0;
}

/**
 * 确立最后的长度指针位置和最后的写入指针位置
 */
void restore_last_position_clogan(cLogan_model *model) {
    unsigned char *temp = model->last_point;
    *temp = LOGAN_WRITE_PROTOCOL_HEADER;
    model->total_len++;
    temp++;
    model->content_lent_point = temp; // 内容的指针地址
    *temp = model->content_len >> 24;
    model->total_len++;
    temp++;
    *temp = model->content_len >> 16;
    model->total_len++;
    temp++;
    *temp = model->content_len >> 8;
    model->total_len++;
    temp++;
    *temp = model->content_len;
    model->total_len++;
    temp++;
    model->last_point = temp;

    printf_clogan("restore_last_position_clogan > content_len : %d\n", model->content_len);
}

int clogan_open(const char *pathname) {
    int back = CLOGAN_OPEN_FAIL_NOINIT;
    if (!is_init_ok) {
        back = CLOGAN_OPEN_FAIL_NOINIT;
        return back;
    }

    is_open_ok = 0;
    if (NULL == pathname || 0 == strnlen(pathname, 128) || NULL == _logan_buffer ||
        NULL == _dir_path ||
        0 == strnlen(_dir_path, 128)) {
        back = CLOGAN_OPEN_FAIL_HEADER;
        return back;
    }

    if (NULL != logan_model) { //回写到日志中
        if (logan_model->total_len > LOGAN_WRITEPROTOCOL_HEAER_LENGTH) {
            clogan_flush();
        }
        if (logan_model->file_stream_type == LOGAN_FILE_OPEN) {
            fclose(logan_model->file);
            logan_model->file_stream_type = LOGAN_FILE_CLOSE;
        }
        if (NULL != logan_model->file_path) {
            free(logan_model->file_path);
            logan_model->file_path = NULL;
        }
        logan_model->total_len = 0;
    } else {
        logan_model = malloc(sizeof(cLogan_model));
        if (NULL != logan_model) {
            memset(logan_model, 0, sizeof(cLogan_model));
        } else {
            logan_model = NULL; //初始化Logan_model失败,直接退出
            is_open_ok = 0;
            back = CLOGAN_OPEN_FAIL_MALLOC;
            return back;
        }
    }
    char *temp = NULL;
    if (_file_name == NULL) {
        _file_name = (char *) malloc(strlen(pathname) + 1);
        strcpy(_file_name, pathname);
        LOGI("clogan_write _file_name init :[%s]", _file_name);
        char *last = getDayLastFileName(_dir_path);
        if (last != NULL && logan_model->file_path == NULL) {
            logan_model->file_path = last;
        }
    }
    size_t file_path_len = strlen(_dir_path) + strlen(pathname) + 1;
    char *temp_file = malloc(file_path_len); // 日志文件路径
    if (NULL != temp_file) {
        memset(temp_file, 0, file_path_len);
        temp = temp_file;
        memcpy(temp, _dir_path, strlen(_dir_path));
        temp += strlen(_dir_path);
        memcpy(temp, pathname, strlen(pathname)); //创建文件路径

        if (logan_model->file_path == NULL) {
            logan_model->file_path = temp_file;
        }
        LOGI(" logan_model->file_path: [%s]", logan_model->file_path);

        if (!init_file_clogan(logan_model)) {  //初始化文件IO和文件大小
            is_open_ok = 0;
            back = CLOGAN_OPEN_FAIL_IO;
            return back;
        }

        if (init_zlib_clogan(logan_model) != Z_OK) { //初始化zlib压缩
            is_open_ok = 0;
            back = CLOGAN_OPEN_FAIL_ZLIB;
            return back;
        }

        logan_model->buffer_point = _logan_buffer;

        if (buffer_type == LOGAN_MMAP_MMAP) {  //如果是MMAP,缓存文件目录和文件名称
            cJSON *root = NULL;
            Json_map_logan *map = NULL;
            root = cJSON_CreateObject();
            map = create_json_map_logan();
            char *back_data = NULL;
            if (NULL != root) {
                if (NULL != map) {
                    add_item_number_clogan(map, LOGAN_VERSION_KEY, CLOGAN_VERSION_NUMBER);
                    add_item_string_clogan(map, LOGAN_PATH_KEY, logan_model->file_path);
                    inflate_json_by_map_clogan(root, map);
                    back_data = cJSON_PrintUnformatted(root);
                }
                cJSON_Delete(root);
                if (NULL != back_data) {
                    add_mmap_header_clogan(back_data, logan_model);
                    free(back_data);
                } else {
                    logan_model->total_point = _logan_buffer;
                    logan_model->total_len = 0;
                }
            } else {
                logan_model->total_point = _logan_buffer;
                logan_model->total_len = 0;
            }

            logan_model->last_point = logan_model->total_point + LOGAN_MMAP_TOTALLEN;

            if (NULL != map) {
                delete_json_map_clogan(map);
            }
        } else {
            logan_model->total_point = _logan_buffer;
            logan_model->total_len = 0;
            logan_model->last_point = logan_model->total_point + LOGAN_MMAP_TOTALLEN;
        }
        restore_last_position_clogan(logan_model);
        init_encrypt_key_clogan(logan_model);
        logan_model->is_ok = 1;
        is_open_ok = 1;
    } else {
        is_open_ok = 0;
        back = CLOGAN_OPEN_FAIL_MALLOC;
        printf_clogan("clogan_open > malloc memory fail\n");
    }

    if (is_open_ok) {
        back = CLOGAN_OPEN_SUCCESS;
        printf_clogan("clogan_open > logan open success\n");
    } else {
        printf_clogan("clogan_open > logan open fail\n");
    }
    return back;
}


//更新总数据和最后的count的数据到内存中
void update_length_clogan(cLogan_model *model) {
    unsigned char *temp = NULL;
    if (NULL != model->total_point) {
        temp = model->total_point;
        *temp = model->total_len;
        temp++;
        *temp = model->total_len >> 8;
        temp++;
        *temp = model->total_len >> 16;
    }

    if (NULL != model->content_lent_point) {
        temp = model->content_lent_point;
        // 为了兼容java,采用高字节序
        *temp = model->content_len >> 24;
        temp++;
        *temp = model->content_len >> 16;
        temp++;
        *temp = model->content_len >> 8;
        temp++;
        *temp = model->content_len;
    }
}

//对clogan_model数据做还原
void clear_clogan(cLogan_model *logan_model) {
    logan_model->total_len = 0;

    if (logan_model->zlib_type == LOGAN_ZLIB_END) { //因为只有ZLIB_END才会释放掉内存,才能再次初始化
        memset(logan_model->strm, 0, sizeof(z_stream));
        logan_model->zlib_type = LOGAN_ZLIB_NONE;
        init_zlib_clogan(logan_model);
    }
    logan_model->remain_data_len = 0;
    logan_model->content_len = 0;
    logan_model->last_point = logan_model->total_point + LOGAN_MMAP_TOTALLEN;
    restore_last_position_clogan(logan_model);
    init_encrypt_key_clogan(logan_model);
    logan_model->total_len = 0;
    update_length_clogan(logan_model);
    logan_model->total_len = LOGAN_WRITEPROTOCOL_HEAER_LENGTH;
}

//对空的文件插入一行头文件做标示
void insert_header_file_clogan(cLogan_model *loganModel) {
    char *log = "clogan header";
    int flag = 1;
    long long local_time = get_system_current_clogan();
    char *thread_name = "clogan";
    long long thread_id = 1;
    int ismain = 1;
    Construct_Data_cLogan *data = construct_json_data_clogan(log, flag, local_time, thread_name,
                                                             thread_id, ismain);
    if (NULL == data) {
        return;
    }
    cLogan_model temp_model; //临时的clogan_model
    int status_header = 1;
    memset(&temp_model, 0, sizeof(cLogan_model));
    if (Z_OK != init_zlib_clogan(&temp_model)) {
        status_header = 0;
    }

    if (status_header) {
        init_encrypt_key_clogan(&temp_model);
        int length = data->data_len * 10;
        unsigned char temp_memory[length];
        memset(temp_memory, 0, length);
        temp_model.total_len = 0;
        temp_model.last_point = temp_memory;
        restore_last_position_clogan(&temp_model);
        clogan_zlib_compress(&temp_model, data->data, data->data_len);
        clogan_zlib_end_compress(&temp_model);
        update_length_clogan(&temp_model);

        fwrite(temp_memory, sizeof(char), temp_model.total_len, loganModel->file);//写入到文件中
        fflush(logan_model->file);
        loganModel->file_len += temp_model.total_len; //修改文件大小
    }

    if (temp_model.is_malloc_zlib) {
        free(temp_model.strm);
        temp_model.is_malloc_zlib = 0;
    }
    construct_data_delete_clogan(data);
}

//文件写入磁盘、更新文件大小
void write_dest_clogan(void *point, size_t size, size_t length, cLogan_model *loganModel) {
    if (!is_file_exist_clogan(loganModel->file_path)) { //如果文件被删除,再创建一个文件
        if (logan_model->file_stream_type == LOGAN_FILE_OPEN) {
            fclose(logan_model->file);
            logan_model->file_stream_type = LOGAN_FILE_CLOSE;
        }
        if (NULL != _dir_path) {
            if (!is_file_exist_clogan(_dir_path)) {
                makedir_clogan(_dir_path);
            }
            init_file_clogan(logan_model);
            printf_clogan("clogan_write > create log file , restore open file stream \n");
        }
    }
    if (CLOGAN_EMPTY_FILE == loganModel->file_len) { //如果是空文件插入一行CLogan的头文件
        insert_header_file_clogan(loganModel);
    }
    fwrite(point, sizeof(char), logan_model->total_len, logan_model->file);//写入到文件中
    fflush(logan_model->file);
    loganModel->file_len += loganModel->total_len; //修改文件大小
}

void write_flush_clogan() {
    if (logan_model->zlib_type == LOGAN_ZLIB_ING) {
        clogan_zlib_end_compress(logan_model);
        update_length_clogan(logan_model);
    }
    if (logan_model->total_len > LOGAN_WRITEPROTOCOL_HEAER_LENGTH) {
        unsigned char *point = logan_model->total_point;
        point += LOGAN_MMAP_TOTALLEN;
        write_dest_clogan(point, sizeof(char), logan_model->total_len, logan_model);
        printf_clogan("write_flush_clogan > logan total len : %d \n", logan_model->total_len);
        clear_clogan(logan_model);
    }
}

void clogan_write2(char *data, int length) {
    if (NULL != logan_model && logan_model->is_ok) {
        clogan_zlib_compress(logan_model, data, length);
        update_length_clogan(logan_model); //有数据操作,要更新数据长度到缓存中
        int is_gzip_end = 0;

        if (!logan_model->file_len ||
            logan_model->content_len >= LOGAN_MAX_GZIP_UTIL) { //是否一个压缩单元结束
            clogan_zlib_end_compress(logan_model);
            is_gzip_end = 1;
            update_length_clogan(logan_model);
        }

        int isWrite = 0;
        if (!logan_model->file_len && is_gzip_end) { //如果是个空文件、第一条日志写入
            isWrite = 1;
            printf_clogan("clogan_write2 > write type empty file \n");
        } else if (buffer_type == LOGAN_MMAP_MEMORY && is_gzip_end) { //直接写入文件
            isWrite = 1;
            printf_clogan("clogan_write2 > write type memory \n");
        } else if (buffer_type == LOGAN_MMAP_MMAP &&
                   logan_model->total_len >=
                   buffer_length / LOGAN_WRITEPROTOCOL_DEVIDE_VALUE) { //如果是MMAP 且 文件长度已经超过三分之一
            isWrite = 1;
            printf_clogan("clogan_write2 > write type MMAP \n");
        }
        if (isWrite) { //写入
            write_flush_clogan();
        } else if (is_gzip_end) { //如果是mmap类型,不回写IO,初始化下一步
            logan_model->content_len = 0;
            logan_model->remain_data_len = 0;
            init_zlib_clogan(logan_model);
            restore_last_position_clogan(logan_model);
            init_encrypt_key_clogan(logan_model);
        }
    }
}

//如果数据流非常大,切割数据,分片写入
void clogan_write_section(char *data, int length) {
    int size = LOGAN_WRITE_SECTION;
    int times = length / size;
    int remain_len = length % size;
    char *temp = data;
    int i = 0;
    for (i = 0; i < times; i++) {
        clogan_write2(temp, size);
        temp += size;
    }
    if (remain_len) {
        clogan_write2(temp, remain_len);
    }
}

/**
 @brief 写入数据 按照顺序和类型传值(强调、强调、强调)
 @param flag 日志类型 (int)
 @param log 日志内容 (char*)
 @param local_time 日志发生的本地时间，形如1502100065601 (long long)
 @param thread_name 线程名称 (char*)
 @param thread_id 线程id (long long) 为了兼容JAVA
 @param ismain 是否为主线程，0为是主线程，1位非主线程 (int)
 */
int
clogan_write(int flag, char *log, long long local_time, char *thread_name, long long thread_id,
             int is_main) {
    int back = CLOGAN_WRITE_FAIL_HEADER;
    if (!is_init_ok || NULL == logan_model || !is_open_ok) {
        back = CLOGAN_WRITE_FAIL_HEADER;
        return back;
    }

    if (logan_model->file_len > max_file_len) {
        _file_count = getDayFileCount(_dir_path);
        if (_file_count >= max_file_count) {
            delete(_dir_path);
            printf_clogan("clogan_write > beyond max file , cant write log\n");
//            back = CLOAGN_WRITE_FAIL_MAXFILE;
//            return back;
        }
        LOGI("clogan_write file_count:[%d]", _file_count);
        char *format_time_string = NULL;
        format_time_string = (char *) malloc(128);
        get_format_time_string(format_time_string);

        char *nextFile = (char *) malloc(
                strlen(_file_name) + strlen("_") + strlen(format_time_string) + 1);
        strcpy(nextFile, _file_name);
        strcat(nextFile, "_");
        strcat(nextFile, format_time_string);
        LOGI("clogan_write nextFile:[%s]", nextFile);
        free(format_time_string);
        clogan_open(nextFile);
        free(nextFile);
    }

    //判断MMAP文件是否存在,如果被删除,用内存缓存
    if (buffer_type == LOGAN_MMAP_MMAP && !is_file_exist_clogan(_mmap_file_path)) {
        if (NULL != _cache_buffer_buffer) {
            buffer_type = LOGAN_MMAP_MEMORY;
            buffer_length = LOGAN_MEMORY_LENGTH;

            printf_clogan("clogan_write > change to memory buffer");

            _logan_buffer = _cache_buffer_buffer;
            logan_model->total_point = _logan_buffer;
            logan_model->total_len = 0;
            logan_model->content_len = 0;
            logan_model->remain_data_len = 0;

            if (logan_model->zlib_type == LOGAN_ZLIB_INIT) {
                clogan_zlib_delete_stream(logan_model); //关闭已开的流
            }

            logan_model->last_point = logan_model->total_point + LOGAN_MMAP_TOTALLEN;
            restore_last_position_clogan(logan_model);
            init_zlib_clogan(logan_model);
            init_encrypt_key_clogan(logan_model);
            logan_model->is_ok = 1;
        } else {
            buffer_type = LOGAN_MMAP_FAIL;
            is_init_ok = 0;
            is_open_ok = 0;
            _logan_buffer = NULL;
        }
    }

    Construct_Data_cLogan *data = construct_json_data_clogan(log, flag, local_time, thread_name,
                                                             thread_id, is_main);
    if (NULL != data) {
        clogan_write_section(data->data, data->data_len);
        construct_data_delete_clogan(data);
        back = CLOGAN_WRITE_SUCCESS;
    } else {
        back = CLOGAN_WRITE_FAIL_MALLOC;
    }
    return back;
}


int clogan_flush(void) {
    int back = CLOGAN_FLUSH_FAIL_INIT;
    if (!is_init_ok || NULL == logan_model) {
        return back;
    }
    write_flush_clogan();
    back = CLOGAN_FLUSH_SUCCESS;
    printf_clogan(" clogan_flush > write flush\n");
    return back;
}

void clogan_debug(int debug) {
    set_debug_clogan(debug);
}
