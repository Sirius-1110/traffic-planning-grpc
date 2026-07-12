#include "TNM_utility.h"

int TNM_FloatFormat::sWidth = 12;
int TNM_FloatFormat::sDigit = 6;
int TNM_IntFormat::sWidth = 8;

bool TNM_OpenInFile(ifstream& input, const string& file) {
    input.open(file.c_str(), ios::in);
    if (!input) {
        cout << "Cannot open file " << file << " to read" << endl;
        return false;
    }
    return true;
}

bool TNM_OpenOutFile(ofstream& output, const string& file) {
    output.open(file.c_str(), ios::out);
    if (!output) {
        cout << "Cannot open file " << file << " to write" << endl;
        return false;
    }
    return true;
}

ifstream& TNM_SkipString(ifstream& input, int count) {
    string ignored;
    for (int i = 0; i < count; ++i) {
        input >> ignored;
    }
    return input;
}

void TNM_GetWordsFromLine(
    const string& line,
    vector<string>& words,
    const char delimiter,
    const char exception) {
    words.clear();
    string current;
    bool quoted = false;

    for (char ch : line) {
        if (exception != ' ' && ch == exception) {
            quoted = !quoted;
            continue;
        }
        if (ch == delimiter && !quoted) {
            words.push_back(current);
            current.clear();
        } else if (ch != '\r') {
            current.push_back(ch);
        }
    }
    words.push_back(current);
}

void TNM_GetWordsFromLine(string& line, vector<string>& words) {
    words.clear();
    istringstream input(line);
    string word;
    while (input >> word) {
        words.push_back(word);
    }
}

string GetSourcePath() {
    return ".";
}

TNM_FloatFormat::TNM_FloatFormat(const floatType x, int w, int d)
    : num(x), width(w), digit(d) {}

TNM_FloatFormat::TNM_FloatFormat(const floatType x, int w)
    : num(x), width(w), digit(sDigit) {}

TNM_FloatFormat::TNM_FloatFormat(const floatType x)
    : num(x), width(sWidth), digit(sDigit) {}

ostream& TNM_FloatFormat::print(ostream& output) const {
    output << setw(width) << setprecision(digit) << fixed << num;
    return output;
}

ostream& operator<<(ostream& output, const TNM_FloatFormat& value) {
    return value.print(output);
}

TNM_IntFormat::TNM_IntFormat(const int x, int w) : num(x), width(w) {}
TNM_IntFormat::TNM_IntFormat(const int x) : num(x), width(sWidth) {}

ostream& TNM_IntFormat::print(ostream& output) const {
    output << setw(width) << static_cast<int>(num);
    return output;
}

ostream& operator<<(ostream& output, const TNM_IntFormat& value) {
    return value.print(output);
}

floatType TNM_Position::GetDist(TNM_Position* pos, char) {
    if (!pos) {
        return 0;
    }
    const floatType lat1 = deg2rad(m_lat);
    const floatType lat2 = deg2rad(pos->m_lat);
    const floatType lon_delta = deg2rad(m_lon - pos->m_lon);
    const floatType cosine =
        sin(lat1) * sin(lat2) + cos(lat1) * cos(lat2) * cos(lon_delta);
    const floatType bounded = max<floatType>(-1, min<floatType>(1, cosine));
    return rad2deg(acos(bounded)) * 60;
}
