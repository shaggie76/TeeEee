#ifndef MOVIES_H
#define MOVIES_H

#include <ctime>
#include <vector>

struct Movie
{
    TCHAR   name[MAX_PATH];
    TCHAR   path[MAX_PATH];
    TCHAR   coverPath[MAX_PATH];

    HBITMAP cover;
    
    enum State
    {
        MS_DORMANT,
        MS_LOADING,
        MS_LOADED
    };
    
    volatile State state;
};

typedef std::vector<Movie> Movies;

extern Movies gMovies;

extern Movies gShush;
extern Movies gTimeout;
extern Movies gLoading;
extern Movies gGoodnight;

extern void FindMovies();
extern void UnloadMovie(Movie& movie);

extern TCHAR* FindExtension(TCHAR* str);
extern TCHAR* FindBaseName(TCHAR* str);

#endif // MOVIES_H
