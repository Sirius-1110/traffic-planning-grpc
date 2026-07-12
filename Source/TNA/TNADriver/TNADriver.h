#ifndef TNADRIVER_H

#define  TNADRIVER_H


#include <iostream>
#include <tchar.h>
//#include <mysql.h>
#include "../../../Include/postgresql/libpq-fe.h"
#include "TNADriver.h"
#include <typeinfo>



#include <algorithm>
#include <math.h>

using namespace std;	

// Progress callback signature: (iter, precision, percent 0..100)
typedef void (*TNADriver_ProgressCallback)(int, double, int);

extern "C" __declspec(dllexport) int TestGreedy_dijk(const string& path, const string& name);
//extern "C" __declspec(dllexport) int TestGreedy_dijkWithPostgreSQL(const string& dbConnStr);
extern "C" __declspec(dllexport) const char* TestGreedy_dijkWithPostgreSQL(const char* jsonConfig, const char* networkTableName, const char* odTableName);
extern "C" __declspec(dllexport) const char* TestGreedy_dijkWithPostgreSQLEx(TNADriver_ProgressCallback progressFunc, const char* jsonConfig, const char* networkTableName, const char* odTableName);
extern "C" __declspec(dllexport) int TestSimpleFunction();
#endif