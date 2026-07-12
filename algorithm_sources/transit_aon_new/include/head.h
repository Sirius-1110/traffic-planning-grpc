#pragma once
#include <string>
#include <iostream>
#include <istream>
#include <fstream>
#include <sstream>

#include <cmath>
#include <limits.h>
#include <ctime>
#include <iterator>
#include <iomanip>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <stack>
#include <queue>
#include <list>   

#ifdef __min
#undef __min
#endif

#ifdef __max
#undef __max
#endif

template <typename T>
T __min(const T& a, const T& b) {
	return std::min(a, b);
}

template <typename T>
T __min(const T& a, const T& b, const T& c) {
	return std::min(a, std::min(b, c));
}

template <typename T>
T __max(const T& a, const T& b) {
	return std::max(a, b);
}

template <typename T>
T __max(const T& a, const T& b, const T& c) {
	return std::max(a, std::max(b, c));
}


using namespace std;

typedef long double floatType;
typedef char   tinyInt;  // 0 ~ 255, one byte
typedef int    smallInt; // 0 ~ 65,535, two byte
typedef long   largeInt; // 0 ~ 4, 294,967,295, four byte

#pragma once
const double	POS_INF_FLOAT = 1e15;
const double	PI = 3.14159265358979323846;


#define PTNet_API




