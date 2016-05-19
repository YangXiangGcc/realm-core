#include <ctime>
#include <cstring>
#include <algorithm>
#include <locale>
#include <limits>       // quiet_NaN
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <float.h> // DBL_MIN, DBL_MAX

#include <unistd.h> // link, unlink

#include <realm/util/file.hpp>

#include "timer.hpp"
#include "results.hpp"

using namespace realm;
using namespace test_util;


namespace {

std::string format_elapsed_time(double seconds)
{
    std::ostringstream out;
    Timer::format(seconds, out);
    return out.str();
}

std::string format_change_percent(double baseline, double seconds)
{
    std::ostringstream out;
    double percent = (seconds - baseline) / baseline * 100;
    out.precision(2);
    out.setf(std::ios_base::showpos);
    out << std::fixed;
    out << percent << "%";
    return out.str();
}

std::string format_drop_factor(double baseline, double seconds)
{
    std::ostringstream out;
    double factor = baseline / seconds;
    out.precision(3);
    out << factor << ":1";
    return out.str();
}

std::string format_rise_factor(double baseline, double seconds)
{
    std::ostringstream out;
    double factor = seconds / baseline;
    out.precision(3);
    out << "1:" << factor;
    return out.str();
}

std::string format_change(double baseline, double input,
                          Results::ChangeType change_type)
{
    std::string str;
    switch (change_type) {
        case Results::change_Percent:
            str = format_change_percent(baseline, input);
            break;
        case Results::change_DropFactor:
            str = format_drop_factor(baseline, input);
            break;
        case Results::change_RiseFactor:
            str = format_rise_factor(baseline, input);
            break;
    }
    std::ostringstream os;
    os << '(' << str << ')';
    return os.str();
}

std::string pad_right(std::string str, int width, char padding = ' ')
{
    std::ostringstream ss;
    ss << std::setw(width);
    ss << std::setfill(padding);
    ss << std::left;
    ss << str;
    return ss.str();
}

bool almost_equal(double x, double y)
{
    // Specialized the example in
    // http://en.cppreference.com/w/cpp/types/numeric_limits/epsilon

    return std::abs(x-y) < std::numeric_limits<double>::epsilon() * std::abs(x+y)
           || std::abs(x-y) < std::numeric_limits<double>::min();
}

// Assumes samples is sorted and samples.size() >= 1.
double findMode(std::vector<double>& samples)
{
    size_t count, better_count;
    double mode, better_mode;

    mode = better_mode = samples[0];
    count = better_count = 1;
    for (double sample : samples) {
        if (almost_equal(sample, mode)) {
            count++;
            if (count > better_count) {
                better_mode = mode;
                better_count = count;
            }
        } else {
            mode = sample;
            count = 1;
        }
    }

    return better_mode;
}

// Assumes samples is sorted.
double tryFindMode(std::vector<double>& samples)
{
    return samples.size() ?
        findMode(samples) :
        std::numeric_limits<double>::quiet_NaN();
}

} // anonymous namespace

Results::Result::Result():
    min(DBL_MAX), max(DBL_MIN), total(0), rep(0)
{
}

double Results::Result::avg() const
{
    return total / rep;
}

void Results::submit_single(const char* ident, const char* lead_text,
    double seconds, ChangeType change_type)
{
    submit(ident, seconds);
    finish(ident, lead_text, change_type);
}

void Results::submit(const char* ident, double seconds)
{
    Measurements::iterator it = m_measurements.find(ident);
    if (it == m_measurements.end()) {
        it = m_measurements.insert(std::make_pair(ident, Measurement())).first;
    }
    Measurement& r = it->second;
    r.samples.push_back(seconds);
}

Results::Result Results::Measurement::finish() const
{
    Result r;

    std::vector<double> samples = this->samples;
    // Sort to simplify calculating min/max/median/mode.
    std::sort(samples.begin(), samples.end());

    // Compute total:
    r.total = 0;
    for (double s : samples) {
        r.total += s;
    }

    // Compute min/max/median
    r.rep = samples.size();
    if (r.rep > 0) {
        r.min = samples.front();
        r.max = samples.back();

        if (r.rep % 2 == 0) {
            // Equal number of elements: median is the average of the
            // two middle elements.
            r.median = (samples[r.rep / 2] + samples[r.rep / 2 + 1]) / 2;
        }
        else {
            // Odd number of elements: median is the middle element.
            r.median = samples[r.rep / 2];
        }
    }

    // Calculate standard deviation
    if (r.rep > 1) {
        double mean = r.avg();
        double sum_variance = 0.0;
        for (double s : samples) {
            double x = s - mean;
            sum_variance += x * x;
        }

        // Subtract one because this is a "sample standard deviation" (Bessel's Correction)
        // See: http://en.wikipedia.org/wiki/Bessel%27s_correction
        double n = double(r.rep - 1);

        double variance = sum_variance / n;
        r.stddev = std::sqrt(variance);
        REALM_ASSERT_RELEASE(r.stddev != 0);
    }
    else {
        r.stddev = 0;
    }

    r.mode = tryFindMode(samples);

    return r;
}

void Results::finish(const std::string& ident,
                              const std::string& lead_text,
                              ChangeType change_type)
{
    /*
        OUTPUT FOR RESULTS WITHOUT BASELINE:
        Lead Text             min 0.0s     max 0.0s    avg 0.0s
        Lead Text 2           min 123.0s   max 32.0s   avg 1.0s

        OUTPUT FOR RESULTS WITH BASELINE:
        Lead Text             min 0.0s (+10%)   max 0.0s (-20%)   avg 0.0s (0%)
        Lead Text 2           min 0.0s (+10%)   max 0.0s (-20%)   avg 0.0s (0%)
    */

    BaselineResults::const_iterator baseline_iter =
        m_baseline_results.find(ident);

    std::ostream& out = std::cout;

    // Print Lead Text
    out.setf(std::ios_base::left, std::ios_base::adjustfield);
    m_max_lead_text_width = std::max(m_max_lead_text_width,
                                     int(lead_text.size()));
    std::string lead_text_2 = lead_text + ":";
    out << std::setw(m_max_lead_text_width + 1 + 3) << lead_text_2;

    Measurements::const_iterator it = m_measurements.find(ident);
    if (it == m_measurements.end()) {
        out << "(no measurements)" << std::endl;
        return;
    }

    Result r = it->second.finish();

    const size_t time_width = 8;

    out.setf(std::ios_base::right, std::ios_base::adjustfield);
    if (baseline_iter != m_baseline_results.end()) {
        const Result& br = baseline_iter->second;
        double avg = r.avg();
        double baseline_avg = br.avg();

        if ((r.min - br.min) > r.stddev*2) {
            out << "* ";
        }
        else {
            out << "  ";
        }

        out << std::endl << "  ";

        out << "min "
            << std::setw(time_width)
            << format_elapsed_time(r.min)
            << " "
            << pad_right(format_change(br.min, r.min, change_type), 15)
            << "     ";

        out << "max "
            << std::setw(time_width)
            << format_elapsed_time(r.max)
            << " " << pad_right(format_change(br.max, r.max, change_type), 15)
            << "   ";

        if ((r.median - br.median) > r.stddev*2) {
            out << "* ";
        }
        else {
            out << "  ";
        }

        out << std::endl << "  ";

        out << "med "
            << std::setw(time_width)
            << format_elapsed_time(r.median)
            << " "
            << pad_right(format_change(br.median, r.median, change_type), 15)
            << "   ";

        out << "mode "
            << std::setw(time_width)
            << format_elapsed_time(r.mode)
            << " "
            << pad_right(format_change(br.mode, r.mode, change_type), 15)
            << "     ";

        out << std::endl << "  ";

        if ((avg - baseline_avg) > r.stddev*2) {
            out << "* ";
        }
        else {
            out << "  ";
        }

        out << "avg "
            << std::setw(time_width) << format_elapsed_time(avg)
            << " "
            << pad_right(format_change(baseline_avg, avg, change_type), 15)
            << "     ";

        out << "stddev"
            << std::setw(time_width)
            << format_elapsed_time(r.stddev)
            << " "
            << pad_right(format_change(br.stddev, r.stddev, change_type), 15);
    }
    else {

        out << std::endl << "  ";

        out << "min "
            << std::setw(time_width)
            << format_elapsed_time(r.min)
            << "     ";

        out << "max "
            << std::setw(time_width)
            << format_elapsed_time(r.max)
            << "     ";

        out << std::endl << "  ";

        out << "med "
            << std::setw(time_width)
            << format_elapsed_time(r.median)
            << "     ";

        out << "mode "
            << std::setw(time_width)
            << format_elapsed_time(r.mode)
            << "     ";

        out << std::endl << "  ";

        out << "avg "
            << std::setw(time_width)
            << format_elapsed_time(r.avg())
            << "     ";

        out << "stddev "
            << std::setw(time_width)
            << format_elapsed_time(r.stddev);
    }
    out << std::endl;
}


void Results::try_load_baseline_results()
{
    std::string baseline_file = m_results_file_stem;
    baseline_file += ".baseline";
    if (util::File::exists(baseline_file)) {
        std::ifstream in(baseline_file.c_str());
        BaselineResults baseline_results;
        bool error = false;
        std::string line;
        int lineno = 1;
        while (getline(in, line)) {
            std::istringstream line_in(line);
            line_in >> std::skipws;
            std::string ident;
            Result r;
            if (line_in >> ident) {
                double* numbers[] = {&r.min, &r.max, &r.median, &r.mode, &r.stddev, &r.total};
                for (size_t i = 0, length = sizeof(numbers) / sizeof(double); i < length; ++i) {
                    if (!(line_in >> *numbers[i])) {
                        std::cerr << "Expected number: line " << lineno << "\n";
                        error = true;
                        break;
                    }
                }
                if (!error) {
                    if (!(line_in >> r.rep)) {
                        std::cerr << "Expected integer: line " << lineno << "\n";
                        error = true;
                    }
                }
            }
            else {
                std::cerr << "Expected identifier: line " << lineno << "\n";
                error = true;
            }
            if (!line_in) {
                std::cerr << "Unknown error: line " << lineno << '\n';
                error = true;
            }
            if (error)
                break;
            baseline_results[ident] = r;
            ++lineno;
        }
        if (error) {
            std::cerr << "WARNING: Failed to parse '"<<baseline_file<<"'\n";
        }
        else {
            m_baseline_results = baseline_results;
        }
    }
}


void Results::save_results()
{
    time_t now = time(0);
    localtime(&now);
    struct tm local;
    localtime_r(&now,  &local);
    std::ostringstream name_out;
    name_out << m_results_file_stem << ".";
    // Format: YYYYMMDD_hhmmss;
    name_out.fill('0');
    name_out << (1900 + local.tm_year) << ""
        "" << std::setw(2) << (1 + local.tm_mon) << ""
        "" << std::setw(2) << local.tm_mday << "_"
        "" << std::setw(2) << local.tm_hour << ""
        "" << std::setw(2) << local.tm_min << ""
        "" << std::setw(2) << local.tm_sec;
    std::string name = name_out.str();
    std::string csv_name = name + ".csv";
    {
        std::ofstream out(name.c_str());
        std::ofstream csv_out(csv_name.c_str());

        csv_out << "ident,min,max,median,mode,avg,stddev,reps,total" << '\n';
        csv_out.setf(std::ios_base::fixed, std::ios_base::floatfield);

        typedef Measurements::const_iterator iter;
        for (iter it = m_measurements.begin(); it != m_measurements.end(); ++it) {
            Result r = it->second.finish();

            out << it->first << ' ';
            out << r.min << " " << r.max << " " << r.median << " " << r.mode << " " << r.stddev << " " << r.total << " " << r.rep << '\n';

            csv_out << '"' << it->first << "\",";
            csv_out << r.min << ',' << r.max << ',' << r.median << ',' << r.mode << ',' << r.avg() << ',' << r.stddev << ',' << r.rep << ',' << r.total << '\n';
        }
    }

    std::string baseline_file = m_results_file_stem;
    std::string latest_csv_file = m_results_file_stem + ".latest.csv";
    baseline_file += ".baseline";
    int r;
    if (!util::File::exists(baseline_file)) {
        r = link(name.c_str(), baseline_file.c_str());
    }
    if (util::File::exists(latest_csv_file)) {
        r = unlink(latest_csv_file.c_str());
    }
    r = link(csv_name.c_str(), latest_csv_file.c_str());
    static_cast<void>(r); // FIXME: Display if error
}

