# Witch(hunt) Reader

This firmware is based on the [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) for the XTEINK X4, a great piece of software by Dave Allie and others.

# Attributions
If in doubt consider all the work being done here based on the work of others - especially crosspoint reader (as the ancestor of this version) and microreader have been a great source of inspiration.

# What this reader does differently
- Speed - rendering should be *fast*
- CSS layout - a lot of effort have gone into rendering 
- Memory - where others fail Witch Reader still works
- Proper KOReader Snychronisation
- Additional sleep screens support (information overlay, transparent pictures over current reader screen)
- Clock-Support for X4 and X3
- Weather information panel
- Multiple under-the-hood performance improvements
- Book information screen
- Markdown-support
- WiFi captive portal support
- Supporting ~~strikethrough~~, superscript / subscript and tables
- Support for used defined actions on double-click / long-click per button 
- A lot of smaller quality of life improvements 

# What this reader doesn't
* Great UI design is not necessarily/obviously not a forte of mine, so if you look for a polished look and feel, I would recommend going e.g. to CrossInk, a great piece of work by uxJulia
* Support for CJK (Chinese Japanese Korean) - look at https://github.com/aBER0724/crosspoint-reader-cjk
* Right-to-left rendering support (Hebrew, Arabic) - choose the original Crosspoint firmware
* The most memory efficient reader might still be [MicroReader](https://github.com/CidVonHighwind/microreader) by CidVonHighwind

# Why this name
Originally this fork was called CrossPoint++ - it had a small userbase and then I made an honest mistake by reusing the crosspoint fork, providing ample reference in the PRs and the release notes of code origin and authorship but was losing the github commit author information in the progress when I copied code over instead of taking the tedious (and correct) way of cherrypicking the original commit and post-cleanup effort.

Another crosspoint developer approached me pointing this flaw out and I agreed to change the future integration work. What I did not care for was that persons attitude and way of communicating, and I told him so.

Then all hell broke lose, ending up in insults, harassment and plain lies in other forums without even caring for feedback (a good cause for any lawsuit for slander). So I took the repo down and just continued the development for my own benefit.

Still I wanted others to benefit from my progress, so here we are again:

Witch(hunt) Reader (name for obvious reasons) 

- So if you are one of those who felt poorly appreciated: please accept my apologies, that was never my intent, and I have taken a lot of effort to replace code / to properly attribute the origin of code or ideas
- If you are one of those who felt the need to raise a witchhunt, to lie, to libel: just go away - trolls aren't welcome here or more clearly: F*** OFF

# Rendering comparisons
Rendering examples from [Alice in Wonderland](https://www.gutenberg.org/ebooks/28885)
