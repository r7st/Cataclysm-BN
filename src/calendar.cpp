#include "calendar.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>

#include "debug.h"
#include "options.h"
#include "rng.h"
#include "string_formatter.h"
#include "translations.h"

/** How much light moon provides per lit-up quarter (Full-moon light is four times this value) */
static constexpr double moonlight_per_quarter = 2.25;

// Divided by 100 to prevent overflowing when converted to moves
const int calendar::INDEFINITELY_LONG( std::numeric_limits<int>::max() / 100 );
const time_duration calendar::INDEFINITELY_LONG_DURATION(
    time_duration::from_turns( std::numeric_limits<int>::max() ) );
static int cur_season_length = 1;

const time_point calendar::before_time_starts = time_point::from_turn( -1 );
const time_point calendar::turn_zero = time_point::from_turn( 0 );

calendar_config calendar::config = calendar_config( calendar::turn_zero,
                                   calendar::turn_zero,
                                   SPRING,
                                   false );

const time_point &calendar::start_of_cataclysm = calendar::config.start_of_cataclysm();
const time_point &calendar::start_of_game = calendar::config.start_of_game();
const season_type &calendar::initial_season = calendar::config.initial_season();
time_point calendar::turn = calendar::turn_zero;

// Internal constants, not part of the calendar interface.
// Times for sunrise, sunset at equinoxes

/** Hour of sunrise at winter solstice */
static constexpr int sunrise_winter = 7;

/** Hour of sunrise at summer solstice */
static constexpr int sunrise_summer = 5;

/** Hour of sunrise at fall and spring equinox */
static constexpr int sunrise_equinox = ( sunrise_summer + sunrise_winter ) / 2;

/** Hour of sunset at winter solstice */
static constexpr int sunset_winter = 17;

/** Hour of sunset at summer solstice */
static constexpr int sunset_summer = 21;

/** Hour of sunset at fall and spring equinox */
static constexpr int sunset_equinox = ( sunset_summer + sunset_winter ) / 2;

// How long, does sunrise/sunset last?
static const time_duration twilight_duration = 1_hours;

double default_daylight_level()
{
    return 100.0;
}

moon_phase get_moon_phase( const time_point &p )
{
    // Seasons last 14 days, not 91 like in real life
    static constexpr time_duration synodic_month = ( 14 / 3.0f ) * 1_days;
    const time_duration moon_phase_duration =
        calendar::season_ratio() * synodic_month;
    // Reset moon phase at start of the year
    time_point p_year = calendar::turn_zero + time_past_new_year( p );
    // Switch moon phase at noon so it stays the same all night
    const int num_middays = to_days<int>( p_year - calendar::turn_zero + 1_days / 2 );
    const time_duration nearest_midnight = num_middays * 1_days;
    const double phase_change = nearest_midnight / moon_phase_duration;
    const int current_phase = static_cast<int>( std::round( phase_change * MOON_PHASE_MAX ) ) %
                              static_cast<int>( MOON_PHASE_MAX );
    return static_cast<moon_phase>( current_phase );
}

// TODO: Refactor sunrise / sunset
// The only difference between them is the start_hours array
time_point sunrise( const time_point &p )
{
    static_assert( static_cast<int>( SPRING ) == 0,
                   "Expected spring to be the first season.  If not, code below will use wrong index into array" );

    static const std::array<int, 4> start_hours = { { sunrise_equinox, sunrise_summer, sunrise_equinox, sunrise_winter, } };
    const size_t season = static_cast<size_t>( season_of_year( p ) );
    assert( season < start_hours.size() );

    const double start_hour = start_hours[season];
    const double end_hour = start_hours[( season + 1 ) % 4];

    const double into_month = static_cast<double>( day_of_season<int>( p ) ) / to_days<int>
                              ( calendar::season_length() );
    const double time = start_hour * ( 1.0 - into_month ) + end_hour * into_month;

    const time_point midnight = p - time_past_midnight( p );
    return midnight + time_duration::from_minutes( static_cast<int>( time * 60 ) );
}

time_point sunset( const time_point &p )
{
    static_assert( static_cast<int>( SPRING ) == 0,
                   "Expected spring to be the first season.  If not, code below will use wrong index into array" );

    static const std::array<int, 4> start_hours = { { sunset_equinox, sunset_summer, sunset_equinox, sunset_winter, } };
    const size_t season = static_cast<size_t>( season_of_year( p ) );
    assert( season < start_hours.size() );

    const double start_hour = start_hours[season];
    const double end_hour = start_hours[( season + 1 ) % 4];

    const double into_month = static_cast<double>( day_of_season<int>( p ) ) / to_days<int>
                              ( calendar::season_length() );
    const double time = start_hour * ( 1.0 - into_month ) + end_hour * into_month;

    const time_point midnight = p - time_past_midnight( p );
    return midnight + time_duration::from_minutes( static_cast<int>( time * 60 ) );
}

time_point night_time( const time_point &p )
{
    return sunset( p ) + twilight_duration;
}

time_point daylight_time( const time_point &p )
{
    // TODO: Actual daylight should start 18 degrees before sunrise
    return sunrise( p ) + 15_minutes;
}

bool is_night( const time_point &p )
{
    const time_duration now = time_past_midnight( p );
    const time_duration sunrise = time_past_midnight( ::sunrise( p ) );
    const time_duration sunset = time_past_midnight( ::sunset( p ) );

    return now >= sunset + twilight_duration || now <= sunrise;
}

bool is_day( const time_point &p )
{
    const time_duration now = time_past_midnight( p );
    const time_duration sunrise = time_past_midnight( ::sunrise( p ) );
    const time_duration sunset = time_past_midnight( ::sunset( p ) );

    return now >= sunrise + twilight_duration && now <= sunset;
}

bool is_dusk( const time_point &p )
{
    const time_duration now = time_past_midnight( p );
    const time_duration sunset = time_past_midnight( ::sunset( p ) );

    return now >= sunset && now <= sunset + twilight_duration;
}

bool is_dawn( const time_point &p )
{
    const time_duration now = time_past_midnight( p );
    const time_duration sunrise = time_past_midnight( ::sunrise( p ) );

    return now >= sunrise && now <= sunrise + twilight_duration;
}

double current_daylight_level( const time_point &p )
{
    const double percent = static_cast<double>( day_of_season<int>( p ) ) / to_days<int>
                           ( calendar::season_length() );
    double modifier = 1.0;
    // For ~Boston: solstices are +/- 25% sunlight intensity from equinoxes
    static double deviation = 0.25;

    switch( season_of_year( p ) ) {
        case SPRING:
            modifier = 1. + ( percent * deviation );
            break;
        case SUMMER:
            modifier = ( 1. + deviation ) - ( percent * deviation );
            break;
        case AUTUMN:
            modifier = 1. - ( percent * deviation );
            break;
        case WINTER:
            modifier = ( 1. - deviation ) + ( percent * deviation );
            break;
        default:
            debugmsg( "Invalid season" );
    }

    return modifier * default_daylight_level();
}

float sunlight( const time_point &p, const bool vision )
{
    const time_duration now = time_past_midnight( p );
    const time_duration sunrise = time_past_midnight( ::sunrise( p ) );
    const time_duration sunset = time_past_midnight( ::sunset( p ) );

    const double daylight_level = current_daylight_level( p );

    int current_phase = static_cast<int>( get_moon_phase( p ) );
    if( current_phase > static_cast<int>( MOON_PHASE_MAX ) / 2 ) {
        current_phase = static_cast<int>( MOON_PHASE_MAX ) - current_phase;
    }

    const int moonlight = vision ? 1 + static_cast<int>( current_phase * moonlight_per_quarter ) :
                          0;

    if( is_night( p ) ) {
        return moonlight;
    } else if( is_dawn( p ) ) {
        const double percent = ( now - sunrise ) / twilight_duration;
        return static_cast<double>( moonlight ) * ( 1. - percent ) + daylight_level * percent;
    } else if( is_dusk( p ) ) {
        const double percent = ( now - sunset ) / twilight_duration;
        return daylight_level * ( 1. - percent ) + static_cast<double>( moonlight ) * percent;
    } else {
        return daylight_level;
    }
}

static std::string to_string_clipped( const int num, const clipped_unit type,
                                      const clipped_align align )
{
    switch( align ) {
        default:
        case clipped_align::none:
            switch( type ) {
                default:
                case clipped_unit::forever:
                    return _( "forever" );
                case clipped_unit::second:
                    return string_format( vgettext( "%d second", "%d seconds", num ), num );
                case clipped_unit::minute:
                    return string_format( vgettext( "%d minute", "%d minutes", num ), num );
                case clipped_unit::hour:
                    return string_format( vgettext( "%d hour", "%d hours", num ), num );
                case clipped_unit::day:
                    return string_format( vgettext( "%d day", "%d days", num ), num );
                case clipped_unit::week:
                    return string_format( vgettext( "%d week", "%d weeks", num ), num );
                case clipped_unit::season:
                    return string_format( vgettext( "%d season", "%d seasons", num ), num );
                case clipped_unit::year:
                    return string_format( vgettext( "%d year", "%d years", num ), num );
            }
        case clipped_align::right:
            switch( type ) {
                default:
                case clipped_unit::forever:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return _( "    forever" );
                case clipped_unit::second:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d  second", "%3d seconds", num ), num );
                case clipped_unit::minute:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d  minute", "%3d minutes", num ), num );
                case clipped_unit::hour:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d    hour", "%3d   hours", num ), num );
                case clipped_unit::day:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d     day", "%3d    days", num ), num );
                case clipped_unit::week:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d    week", "%3d   weeks", num ), num );
                case clipped_unit::season:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d  season", "%3d seasons", num ), num );
                case clipped_unit::year:
                    //~ Right-aligned time string. should right-align with other strings with this same comment
                    return string_format( vgettext( "%3d    year", "%3d   years", num ), num );
            }
    }
}

std::pair<int, clipped_unit> clipped_time( const time_duration &d )
{
    if( d >= calendar::INDEFINITELY_LONG_DURATION ) {
        return { 0, clipped_unit::forever };
    }

    if( d < 1_minutes ) {
        const int sec = to_seconds<int>( d );
        return { sec, clipped_unit::second };
    } else if( d < 1_hours ) {
        const int min = to_minutes<int>( d );
        return { min, clipped_unit::minute };
    } else if( d < 1_days ) {
        const int hour = to_hours<int>( d );
        return { hour, clipped_unit::hour };
    } else if( d < 7_days ) {
        const int day = to_days<int>( d );
        return { day, clipped_unit::day };
    } else if( d < calendar::season_length() || calendar::eternal_season() ) {
        // eternal seasons means one season is indistinguishable from the next,
        // therefore no way to count them
        const int week = to_weeks<int>( d );
        return { week, clipped_unit::week };
    } else if( d < calendar::year_length() && !calendar::eternal_season() ) {
        // TODO: consider a to_season function, but season length is variable, so
        // this might be misleading
        const int season = to_turns<int>( d ) / to_turns<int>( calendar::season_length() );
        return { season, clipped_unit::season };
    } else {
        // TODO: consider a to_year function, but year length is variable, so
        // this might be misleading
        const int year = to_turns<int>( d ) / to_turns<int>( calendar::year_length() );
        return { year, clipped_unit::year };
    }
}

std::string to_string_clipped( const time_duration &d,
                               const clipped_align align )
{
    const std::pair<int, clipped_unit> time = clipped_time( d );
    return to_string_clipped( time.first, time.second, align );
}

std::string to_string( const time_duration &d )
{
    if( d >= calendar::INDEFINITELY_LONG_DURATION ) {
        return _( "forever" );
    }

    if( d <= 1_minutes ) {
        return to_string_clipped( d );
    }

    time_duration divider = 0_turns;
    if( d < 1_hours ) {
        divider = 1_minutes;
    } else if( d < 1_days ) {
        divider = 1_hours;
    } else if( d < 1_weeks ) {
        divider = 1_days;
    } else if( d < calendar::season_length() || calendar::eternal_season() ) {
        divider = 1_weeks;
    } else if( d < calendar::year_length() ) {
        divider = calendar::season_length();
    } else {
        divider = calendar::year_length();
    }

    if( d % divider != 0_turns ) {
        //~ %1$s - greater units of time (e.g. 3 hours), %2$s - lesser units of time (e.g. 11 minutes).
        return string_format( _( "%1$s and %2$s" ),
                              to_string_clipped( d ),
                              to_string_clipped( d % divider ) );
    }
    return to_string_clipped( d );
}

std::string to_string_approx( const time_duration &dur, const bool verbose )
{
    time_duration d = dur;
    const auto make_result = [verbose]( const time_duration & d, const char *verbose_str,
    const char *short_str ) {
        return string_format( verbose ? verbose_str : short_str, to_string_clipped( d ) );
    };

    time_duration divider = 0_turns;
    time_duration vicinity = 0_turns;

    // Minutes and seconds can be estimated precisely.
    if( d > 1_days ) {
        divider = 1_days;
        vicinity = 2_hours;
    } else if( d > 1_hours ) {
        divider = 1_hours;
        vicinity = 5_minutes;
    }

    if( divider != 0_turns ) {
        const time_duration remainder = d % divider;

        if( remainder >= divider - vicinity ) {
            d += divider;
        } else if( remainder > vicinity ) {
            if( remainder < divider / 2 ) {
                //~ %s - time (e.g. 2 hours).
                return make_result( d, _( "more than %s" ), ">%s" );
            } else {
                //~ %s - time (e.g. 2 hours).
                return make_result( d + divider, _( "less than %s" ), "<%s" );
            }
        }
    }
    //~ %s - time (e.g. 2 hours).
    return make_result( d, _( "about %s" ), "%s" );
}

std::string to_string_time_of_day( const time_point &p )
{
    const int hour = hour_of_day<int>( p );
    const int minute = minute_of_hour<int>( p );
    const int second = ( to_seconds<int>( time_past_midnight( p ) ) ) % 60;
    const std::string format_type = get_option<std::string>( "24_HOUR" );

    if( format_type == "military" ) {
        return string_format( "%02d%02d.%02d", hour, minute, second );
    } else if( format_type == "24h" ) {
        //~ hour:minute (24hr time display)
        return string_format( _( "%02d:%02d:%02d" ), hour, minute, second );
    } else {
        int hour_param = hour % 12;
        if( hour_param == 0 ) {
            hour_param = 12;
        }
        // Padding is removed as necessary to prevent clipping with SAFE notification in wide sidebar mode
        const std::string padding = hour_param < 10 ? " " : "";
        if( hour < 12 ) {
            return string_format( _( "%d:%02d:%02d%sAM" ), hour_param, minute, second, padding );
        } else {
            return string_format( _( "%d:%02d:%02d%sPM" ), hour_param, minute, second, padding );
        }
    }
}

weekdays day_of_week( const time_point &p )
{
    /* Design rationale:
     * <kevingranade> here's a question
     * <kevingranade> what day of the week is day 0?
     * <wito> Sunday
     * <GlyphGryph> Why does it matter?
     * <GlyphGryph> For like where people are and stuff?
     * <wito> 7 is also Sunday
     * <kevingranade> NOAA weather forecasts include day of week
     * <GlyphGryph> Also by day0 do you mean the day people start day 0
     * <GlyphGryph> Or actual day 0
     * <kevingranade> good point, turn 0
     * <GlyphGryph> So day 5
     * <wito> Oh, I thought we were talking about week day numbering in general.
     * <wito> Day 5 is a thursday, I think.
     * <wito> Nah, Day 5 feels like a thursday. :P
     * <wito> Which would put the apocalpyse on a saturday?
     * <Starfyre> must be a thursday.  I was never able to get the hang of those.
     * <ZChris13> wito: seems about right to me
     * <wito> kevingranade: add four for thursday. ;)
     * <kevingranade> sounds like consensus to me
     * <kevingranade> Thursday it is */
    const int day_since_cataclysm = to_days<int>( p - calendar::turn_zero );
    static const weekdays start_day = weekdays::THURSDAY;
    const int result = day_since_cataclysm + static_cast<int>( start_day );
    return static_cast<weekdays>( result % 7 );
}

bool calendar::eternal_season()
{
    return config.eternal_season();
}

time_duration calendar::year_length()
{
    return season_length() * 4;
}

time_duration calendar::season_length()
{
    return time_duration::from_days( std::max( cur_season_length, 1 ) );
}
void calendar::set_eternal_season( bool is_eternal )
{
    set_calendar_config(
        calendar_config( start_of_cataclysm,
                         start_of_game,
                         initial_season,
                         is_eternal ) );
}
void calendar::set_season_length( const int dur )
{
    cur_season_length = dur;
}

static constexpr int default_season_length = 14;

float calendar::season_ratio()
{
    return to_days<float>( season_length() ) / default_season_length;
}

bool calendar::once_every( const time_duration &event_frequency )
{
    return ( calendar::turn - calendar::turn_zero ) % event_frequency == 0_turns;
}

std::string calendar::name_season( season_type s )
{
    static const std::array<std::string, 5> season_names_untranslated = {{
            //~First letter is supposed to be uppercase
            std::string( translate_marker( "Spring" ) ),
            //~First letter is supposed to be uppercase
            std::string( translate_marker( "Summer" ) ),
            //~First letter is supposed to be uppercase
            std::string( translate_marker( "Autumn" ) ),
            //~First letter is supposed to be uppercase
            std::string( translate_marker( "Winter" ) ),
            std::string( translate_marker( "End times" ) )
        }
    };
    if( s >= SPRING && s <= WINTER ) {
        return _( season_names_untranslated[ s ] );
    }

    return _( season_names_untranslated[ 4 ] );
}

time_duration rng( time_duration lo, time_duration hi )
{
    return time_duration( rng( lo.turns_, hi.turns_ ) );
}

bool x_in_y( const time_duration &a, const time_duration &b )
{
    return ::x_in_y( to_turns<int>( a ), to_turns<int>( b ) );
}

const std::vector<std::pair<std::string, time_duration>> time_duration::units = { {
        { "turns", 1_turns },
        { "turn", 1_turns },
        { "t", 1_turns },
        { "seconds", 1_seconds },
        { "second", 1_seconds },
        { "s", 1_seconds },
        { "minutes", 1_minutes },
        { "minute", 1_minutes },
        { "m", 1_minutes },
        { "hours", 1_hours },
        { "hour", 1_hours },
        { "h", 1_hours },
        { "days", 1_days },
        { "day", 1_days },
        { "d", 1_days },
        // TODO: maybe add seasons?
        // TODO: maybe add years? Those two things depend on season length!
    }
};

season_type season_of_year( const time_point &p )
{
    // TODO: Explain why is it cached, make the cache explicit, or get rid of caching
    static time_point prev_turn = calendar::before_time_starts;
    static season_type prev_season = calendar::initial_season;

    if( p != prev_turn ) {
        prev_turn = p;
        if( calendar::eternal_season() ) {
            // If we use calendar::start to determine the initial season, and the user shortens the season length
            // mid-game, the result could be the wrong season!
            return prev_season = calendar::initial_season;
        }
        return prev_season = static_cast<season_type>(
                                 to_turn<int>( p ) / to_turns<int>( calendar::season_length() ) % 4
                             );
    }

    return prev_season;
}

std::string to_string( const time_point &p )
{
    const int year = to_turns<int>( p - calendar::turn_zero ) / to_turns<int>
                     ( calendar::year_length() ) + 1;
    const std::string time = to_string_time_of_day( p );
    if( calendar::eternal_season() ) {
        const int day = to_days<int>( time_past_new_year( p ) );
        //~ 1 is the year, 2 is the day (of the *year*), 3 is the time of the day in its usual format
        return string_format( _( "Year %1$d, day %2$d %3$s" ), year, day, time );
    } else {
        const int day = day_of_season<int>( p ) + 1;
        //~ 1 is the year, 2 is the season name, 3 is the day (of the season), 4 is the time of the day in its usual format
        return string_format( _( "Year %1$d, %2$s, day %3$d %4$s" ), year,
                              calendar::name_season( season_of_year( p ) ), day, time );
    }
}

time_point::time_point()
{
    turn_ = 0;
}

void calendar::set_calendar_config( const calendar_config &calendar_config )
{
    calendar::config = calendar_config;
}
