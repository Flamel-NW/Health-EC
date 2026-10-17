#include <dlfcn.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

static bool target(int fd,const char* kind){
    const char* fault=getenv("HEALTHEC_IO_FAULT");
    if(!fault || std::strcmp(fault,kind))return false;
    char name[4096];auto path=std::string("/proc/self/fd/")+std::to_string(fd);
    ssize_t n=readlink(path.c_str(),name,sizeof(name)-1);
    if(n<0)return false;name[n]=0;
    return std::strstr(name,".shard99.bin.")!=nullptr;
}
extern "C" ssize_t write(int fd,const void* data,size_t size){
    auto real=reinterpret_cast<ssize_t(*)(int,const void*,size_t)>(dlsym(RTLD_NEXT,"write"));
    if(target(fd,"write")){errno=ENOSPC;return -1;}return real(fd,data,size);
}
extern "C" int close(int fd){
    auto real=reinterpret_cast<int(*)(int)>(dlsym(RTLD_NEXT,"close"));
    bool fail=target(fd,"close");int result=real(fd);
    if(fail){errno=EIO;return -1;}return result;
}
