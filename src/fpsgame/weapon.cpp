// weapon.cpp: all shooting and effects code, projectile management
#include "game.h"
#include "weaponstats.h"

namespace game
{
	static const int OFFSETMILLIS = 500;
	vec rays[MAXRAYS];

	struct hitmsg
	{
		int target, lifesequence, info1, info2;
		int headshot;
		ivec dir;
	};
	vector<hitmsg> hits;

	VARP(maxdebris, 10, 25, 1000);

	ICOMMAND(getweapon, "", (), intret(player1->gunselect));

	void gunselect(int gun, fpsent *d)
	{
		if(gun!=d->gunselect)
		{
			d->lastgun = d->gunselect;
			addmsg(N_GUNSELECT, "rci", d, gun);
			playsound(S_WEAPLOAD, d == player1 ? NULL : &d->o);
			d->candouble = (gun==GUN_FIST);
			if(d->attacking) d->attacking = false; // no more unlockable scrollwheel shenanigans -Y
		}
		d->gunselect = gun;
	}

	void nextweapon(int dir, bool force = false)
	{
		if(player1->state!=CS_ALIVE) return;
		dir = (dir < 0 ? NUMGUNS-1 : 1);
		int gun = player1->gunselect;
		loopi(NUMGUNS)
		{
			gun = (gun + dir)%NUMGUNS;
			if(force || player1->ammo[gun]) break;
		}
		if(gun != player1->gunselect) gunselect(gun, player1);
		else playsound(S_NOAMMO);
	}
	ICOMMAND(nextweapon, "ii", (int *dir, int *force), nextweapon(*dir, *force!=0));

	int getweapon(const char *name)
	{
		const char *abbrevs[] = { "FI", "MG", "SG", "RI", "CG", "RL", "GL" };
		if(isdigit(name[0])) return parseint(name);
		else loopi(sizeof(abbrevs)/sizeof(abbrevs[0])) if(!strcasecmp(abbrevs[i], name)) return i;
		return -1;
	}
	const char *getweaponname(int gun)
	{
		const char *gunname[] = { "melee", "rifle", "shotgun", "sniper", "chaingun", "rocket launcher", "grenade launcher" };
		return gunname[gun];
	}
	void setweapon(const char *name, bool force = false)
	{
		int gun = getweapon(name);
		if(player1->state!=CS_ALIVE || gun<GUN_FIST || gun>GUN_GL) return;
		if(force || player1->ammo[gun]) gunselect(gun, player1);
		else playsound(S_NOAMMO);
	}
	ICOMMAND(setweapon, "s", (char *name), setweapon(name));

	void cycleweapon(int numguns, int *guns, bool force = false)
	{
		if(numguns<=0 || player1->state!=CS_ALIVE) return;
		int offset = 0;
		loopi(numguns) if(guns[i] == player1->gunselect) { offset = i+1; break; }
		loopi(numguns)
		{
			int gun = guns[(i+offset)%numguns];
			if(gun>=0 && gun<NUMGUNS && (force || player1->ammo[gun]))
			{
				gunselect(gun, player1);
				return;
			}
		}
		playsound(S_NOAMMO);
	}
	ICOMMAND(cycleweapon, "V", (tagval *args, int numargs),
	{
		 int numguns = min(numargs, 7);
		 int guns[7];
		 loopi(numguns) guns[i] = getweapon(args[i].getstr());
		 cycleweapon(numguns, guns);
	});

	VARP(oldweapswitch, 0, 0, 1);

	void weaponswitch(fpsent *d)
	{
		if(d->state!=CS_ALIVE) return;
		if(oldweapswitch) {
			int s = d->gunselect;
			if(s != GUN_CG && d->ammo[GUN_CG])     s = GUN_CG;
			else if(s != GUN_RL && d->ammo[GUN_RL])     s = GUN_RL;
			else if(s != GUN_SG && d->ammo[GUN_SG])     s = GUN_SG;
			else if(s != GUN_RIFLE && d->ammo[GUN_RIFLE])  s = GUN_RIFLE;
			else if(s != GUN_GL && d->ammo[GUN_GL])     s = GUN_GL;
			else if(s != GUN_SMG && d->ammo[GUN_SMG])    s = GUN_SMG;
			else                                          s = GUN_FIST;
			gunselect(s, d);
		}
		else 
		{
			if(d->gunselect != d->lastgun && d->ammo[d->lastgun]) gunselect(d->lastgun, d); // make sure we don't switch to something we don't have ammo for.
			else gunselect(GUN_FIST, d);
		}
	}

	ICOMMAND(weapon, "V", (tagval *args, int numargs),
	{
		if(player1->state!=CS_ALIVE) return;
		loopi(7)
		{
			const char *name = i < numargs ? args[i].getstr() : "";
			if(name[0])
			{
				int gun = getweapon(name);
				if(gun >= GUN_FIST && gun <= GUN_GL && gun != player1->gunselect && player1->ammo[gun]) { gunselect(gun, player1); return; }
			} else { weaponswitch(player1); return; }
		}
		playsound(S_NOAMMO);
	});

	void offsetray(const vec &from, const vec &to, int spread, float range, vec &dest)
	{
		vec offset;
		do offset = vec(rndscale(1), rndscale(1), rndscale(1)).sub(0.5f);
		while(offset.squaredlen() > 0.5f*0.5f);
		offset.mul((to.dist(from)/1024)*spread);
		offset.z /= 2;
		dest = vec(offset).add(to);
		if(dest != from)
		{
			vec dir = vec(dest).sub(from).normalize();
			raycubepos(from, dir, dest, range, RAY_CLIPMAT|RAY_ALPHAPOLY);
		}
	}

	void createrays(int gun, const vec &from, const vec &to)             // create random spread of rays
	{
		loopi(guns[gun].rays) offsetray(from, to, guns[gun].spread, guns[gun].range, rays[i]);
	}

	enum { BNC_GRENADE, BNC_GIBS, BNC_DEBRIS };

	struct bouncer : physent
	{
		int lifetime, bounces;
		float lastyaw, roll;
		bool local;
		fpsent *owner;
		int bouncetype, variant;
		vec offset;
		int offsetmillis;
		float offsetheight;
		int id;
		entitylight light;

		bouncer() : bounces(0), roll(0), variant(0)
		{
			type = ENT_BOUNCE;
		}

		vec offsetpos()
		{
			vec pos(o);
			if(offsetmillis > 0)
			{
				pos.add(vec(offset).mul(offsetmillis/float(OFFSETMILLIS)));
				if(offsetheight >= 0) pos.z = max(pos.z, o.z - max(offsetheight - eyeheight, 0.0f));
			}
			return pos;
		}

		void limitoffset()
		{
			if(bouncetype == BNC_GRENADE && offsetmillis > 0 && offset.z < 0)
				offsetheight = raycube(vec(o.x + offset.x, o.y + offset.y, o.z), vec(0, 0, -1), -offset.z);
			else offsetheight = -1;
		} 
	};

	vector<bouncer *> bouncers;

	vec hudgunorigin(int gun, const vec &from, const vec &to, fpsent *d);

	void newbouncer(const vec &from, const vec &to, bool local, int id, fpsent *owner, int type, int lifetime, int speed, entitylight *light = NULL)
	{
		bouncer &bnc = *bouncers.add(new bouncer);
		bnc.o = from;
		bnc.radius = bnc.xradius = bnc.yradius = type==BNC_DEBRIS ? 0.5f : 1.5f;
		bnc.eyeheight = bnc.radius;
		bnc.aboveeye = bnc.radius;
		bnc.lifetime = lifetime;
		bnc.local = local;
		bnc.owner = owner;
		bnc.bouncetype = type;
		bnc.id = local ? lastmillis : id;
		if(light) bnc.light = *light;

		switch(type)
		{
			case BNC_GRENADE: bnc.collidetype = COLLIDE_ELLIPSE_PRECISE; break;
			case BNC_DEBRIS: bnc.variant = rnd(4); break;
			case BNC_GIBS: bnc.variant = rnd(3); break;
		}

		vec dir(to);
		dir.sub(from).safenormalize();
		bnc.vel = dir;
		bnc.vel.mul(speed);

		avoidcollision(&bnc, dir, owner, 0.1f);

		if(type==BNC_GRENADE)
		{
			bnc.offset = hudgunorigin(GUN_GL, from, to, owner);
			if(owner==followingplayer(player1) && !isthirdperson()) bnc.offset.sub(owner->o).rescale(16).add(owner->o);
		}
		else bnc.offset = from;
		bnc.offset.sub(bnc.o);
		bnc.offsetmillis = OFFSETMILLIS;
		bnc.limitoffset();

		bnc.resetinterp();
	}

	VARP(blood, 0, 1, 1);
	VARP(extrablood, 0, 0, 1);
	HVARP(bloodcolor, 0, 0x9F0000, 0xFFFFFF);

	void bounced(physent *d, const vec &surface)
	{
		if(d->type != ENT_BOUNCE) return;
		bouncer *b = (bouncer *)d;
		if(b->bouncetype != BNC_GIBS || b->bounces >= 2) return;
		b->bounces++;
		unsigned int bloodin = bloodcolor ^ 0xFFFFFF;
		adddecal(DECAL_BLOOD, vec(b->o).sub(vec(surface).mul(b->radius)), surface, 2.96f/b->bounces, bvec((bloodin >> 16) & 0xff, (bloodin >> 8) & 0xff, bloodin & 0xff), rnd(4));
	}
		
	void updatebouncers(int time)
	{
		loopv(bouncers)
		{
			bouncer &bnc = *bouncers[i];
			if(bnc.bouncetype==BNC_GRENADE && bnc.vel.magnitude() > 50.0f)
			{
				vec pos = bnc.offsetpos();
				regular_particle_splash(PART_SMOKE, 1, 150, pos, 0x404040, 2.4f, 50, -20);
			}
			vec old(bnc.o);
			bool stopped = false;
			if(bnc.bouncetype==BNC_GRENADE) stopped = bounce(&bnc, 0.6f, 0.5f, 0.7f) || (bnc.lifetime -= time)<0;
			else
			{
				// cheaper variable rate physics for debris, gibs, etc.
				for(int rtime = time; rtime > 0;)
				{
					int qtime = min(30, rtime);
					rtime -= qtime;
					if((bnc.lifetime -= qtime)<0 || bounce(&bnc, qtime/1000.0f, 0.6f, 0.5f, 1)) { stopped = true; break; }
				}
			}
			if(stopped)
			{
				if(bnc.bouncetype==BNC_GRENADE)
				{
					hits.setsize(0);
					explode(bnc.local, bnc.owner, bnc.o, NULL, guns[GUN_GL].damage, GUN_GL);
					adddecal(DECAL_SCORCH, bnc.o, vec(0, 0, 1), guns[GUN_GL].exprad/2);
					if(bnc.local)
						addmsg(N_EXPLODE, "rci3iv", bnc.owner, lastmillis-maptime, GUN_GL, bnc.id-maptime,
								hits.length(), hits.length()*sizeof(hitmsg)/sizeof(int), hits.getbuf());
				}
				delete bouncers.remove(i--);
			}
			else
			{
				bnc.roll += old.sub(bnc.o).magnitude()/(4*RAD);
				bnc.offsetmillis = max(bnc.offsetmillis-time, 0);
				bnc.limitoffset();
			}
		}
	}

	void removebouncers(fpsent *owner)
	{
		loopv(bouncers) if(bouncers[i]->owner==owner) { delete bouncers[i]; bouncers.remove(i--); }
	}

	void clearbouncers() { bouncers.deletecontents(); }

	struct projectile
	{
		vec dir, o, to, offset;
		float speed;
		fpsent *owner;
		int gun;
		bool local;
		int offsetmillis;
		int id;
		entitylight light;
	};
	vector<projectile> projs;

	void clearprojectiles() { projs.shrink(0); }

	void newprojectile(const vec &from, const vec &to, float speed, bool local, int id, fpsent *owner, int gun)
	{
		projectile &p = projs.add();
		p.dir = vec(to).sub(from).safenormalize();
		p.o = from;
		p.to = to;
		p.offset = hudgunorigin(gun, from, to, owner);
		p.offset.sub(from);
		p.speed = speed;
		p.local = local;
		p.owner = owner;
		p.gun = gun;
		p.offsetmillis = OFFSETMILLIS;
		p.id = local ? lastmillis : id;
	}

	void removeprojectiles(fpsent *owner)
	{
		// can't use loopv here due to strange GCC optimizer bug
		int len = projs.length();
		loopi(len) if(projs[i].owner==owner) { projs.remove(i--); len--; }
	}

	void damageeffect(int damage, fpsent *d, bool thirdperson)
	{
		vec p = d->o;
		p.z += 0.6f*(d->eyeheight + d->aboveeye) - d->eyeheight;
		int blddiv = extrablood ? 1 : 100;
		if(blood) particle_splash(PART_BLOOD, damage/blddiv, 1000, p, bloodcolor ^ 0xFFFFFF, 2.96f);
		if(thirdperson)
		{
			defformatstring(ds, "%d", damage);
			particle_textcopy(d->abovehead(), ds, PART_TEXT, 2000, 0xFF4B19, 4.0f, -8);
		}
	}

	void spawnbouncer(const vec &p, const vec &vel, fpsent *d, int type, entitylight *light = NULL)
	{
		vec to(rnd(100)-50, rnd(100)-50, rnd(100)-50);
		if(to.iszero()) to.z++;
		to.normalize();
		to.add(p);
		newbouncer(p, to, true, 0, d, type, rnd(1000)+1000, rnd(100)+20, light);
	}

	void gibeffect(int damage, const vec &vel, fpsent *d)
	{
		if(!blood || damage <= 0) return;
		vec from = d->abovehead();
		int gibdiv = extrablood ? 25 : 250;
		loopi(min(damage/gibdiv, 40)+1) spawnbouncer(from, vel, d, BNC_GIBS);
	}

	void hit(int damage, dynent *d, fpsent *actor, const vec &vel, int gun, float info1, int info2 = 1, int headshot = 0)
	{
		fpsent* f = (fpsent*)d;
		
		if(actor==player1 && d!=actor && !isteam(actor->team, f->team))
		{
			extern int hitsound, p_hitmark;
			if((hitsound && lasthit != lastmillis && !m_parkour) || (m_parkour && p_hitmark)) playsound(S_HIT);
			lasthit = lastmillis;
		}

		f->lastattacker = actor;
		
		f->lastpain = lastmillis;
		if(actor->type==ENT_PLAYER && !isteam(actor->team, f->team) && f!=actor) actor->totaldamage += damage;

		if(f->type==ENT_AI || f->type==ENT_PLAYER || !m_mp(gamemode)) f->hitpush(damage, vel, actor, gun);

		if(!m_mp(gamemode)) damaged(damage, f, actor, gun);
		else if(!m_parkour)
		{
			hitmsg &h = hits.add();
			h.target = f->clientnum;
			h.lifesequence = f->lifesequence;
			h.info1 = int(info1*DMF);
			h.info2 = info2;
			h.headshot = headshot;
			h.dir = f==actor ? ivec(0, 0, 0) : ivec(vec(vel).mul(DNF));

			if(actor==player1 && f!=player1) damageeffect(damage, f);
		}
	}

	void hitpush(int damage, dynent *d, fpsent *actor, vec &from, vec &to, int gun, int rays, int headshot)
	{
		hit(damage, d, actor, vec(to).sub(from).safenormalize(), gun, from.dist(to), rays, headshot);
	}

	float projdist(dynent *o, vec &dir, const vec &v)
	{
		vec middle = o->o;
		middle.z += (o->aboveeye-o->eyeheight)/2;
		float dist = middle.dist(v, dir);
		dir.div(dist);
		if(dist<0) dist = 0;
		return dist;
	}

	void radialeffect(dynent *o, const vec &v, int qdam, fpsent *actor, int gun)
	{
		if(o->state!=CS_ALIVE) return;
		vec dir;
		float dist = projdist(o, dir, v);
		if(dist<guns[gun].exprad)
		{
			int damage = (int)(qdam*(1-dist/EXP_DISTSCALE/guns[gun].exprad));
			hit(damage, o, actor, dir, gun, dist);
		}
	}

	FVARP(explodebright, 0, 1, 1);

	void explode(bool local, fpsent *owner, const vec &v, dynent *safe, int damage, int gun)
	{
		particle_splash(PART_SPARK, 200, 300, v, 0xB49B4B, 0.24f);
		playsound(gun!=GUN_GL ? S_RLHIT : S_FEXPLODE, &v);
		int color = gun!=GUN_GL ? 0xFF8080 : (m_teammode ? (!strcmp(owner->team, "red") ? 0xFF4040 : 0x4040FF ) : 0xDD40FF);
		if((gun==GUN_RL || gun==GUN_GL) && explodebright < 1) color = vec::hexcolor(color).mul(explodebright).tohexcolor();
		particle_fireball(v, guns[gun].exprad, gun!=GUN_GL ? PART_EXPLOSION : (m_teammode ? (!strcmp(owner->team, "red") ? PART_EXPLOSION_GRENADE_RED : PART_EXPLOSION_GRENADE_BLUE ) : PART_EXPLOSION_GRENADE), gun!=GUN_GL ? -1 : int((guns[gun].exprad-4.0f)*15), color, 4.0f);
		if(gun==GUN_RL) adddynlight(v, 1.15f*guns[gun].exprad, vec(2, 1.5f, 1), 700, 100, 0, guns[gun].exprad/2, vec(1, 0.75f, 0.5f));
		else if(gun==GUN_GL) adddynlight(v, 1.15f*guns[gun].exprad, (m_teammode ? (!strcmp(owner->team, "red") ? vec(1, 0.25f, 0.25f) : vec(0.25f, 0.25f, 1)) : vec(0.8f, 0.25f, 1)), 600, 100, 0, 8);
		else adddynlight(v, 1.15f*guns[gun].exprad, vec(2, 1.5f, 1), 700, 100);
		int numdebris = rnd(maxdebris-5)+5;
		vec debrisvel = vec(owner->o).sub(v).safenormalize(), debrisorigin(v);
		if(gun==GUN_RL) debrisorigin.add(vec(debrisvel).mul(8));
		if(numdebris)
		{
			entitylight light;
			lightreaching(debrisorigin, light.color, light.dir);
			loopi(numdebris)
				spawnbouncer(debrisorigin, debrisvel, owner, BNC_DEBRIS, &light);
		}
		if(!local) return;
		int numdyn = numdynents();
		loopi(numdyn)
		{
			dynent *o = iterdynents(i);
			if(o->o.reject(v, o->radius + guns[gun].exprad) || o==safe) continue;
			radialeffect(o, v, damage, owner, gun);
		}
	}

	void projsplash(projectile &p, vec &v, dynent *safe, int damage)
	{
		if(guns[p.gun].part)
		{
			particle_splash(PART_SPARK, 100, 200, v, 0xB49B4B, 0.24f);
			playsound(S_FEXPLODE, &v);
			// no push?
		}
		else
		{
			explode(p.local, p.owner, v, safe, damage, GUN_RL);
			adddecal(DECAL_SCORCH, v, vec(p.dir).neg(), guns[p.gun].exprad/2);
		}
	}

	void explodeeffects(int gun, fpsent *d, bool local, int id)
	{
		if(local) return;
		switch(gun)
		{
			case GUN_RL:
				loopv(projs)
				{
					projectile &p = projs[i];
					if(p.gun == gun && p.owner == d && p.id == id && !p.local)
					{
						vec pos(p.o);
						pos.add(vec(p.offset).mul(p.offsetmillis/float(OFFSETMILLIS)));
						explode(p.local, p.owner, pos, NULL, 0, GUN_RL);
						adddecal(DECAL_SCORCH, pos, vec(p.dir).neg(), guns[gun].exprad/2);
						projs.remove(i);
						break;
					}
				}
				break;
			case GUN_GL:
				loopv(bouncers)
				{
					bouncer &b = *bouncers[i];
					if(b.bouncetype == BNC_GRENADE && b.owner == d && b.id == id && !b.local)
					{
						vec pos = b.offsetpos();
						explode(b.local, b.owner, pos, NULL, 0, GUN_GL);
						adddecal(DECAL_SCORCH, pos, vec(0, 0, 1), guns[gun].exprad/2);
						delete bouncers.remove(i);
						break;
					}
				}
				break;
			default:
				break;
		}
	}

	bool isheadshot(dynent* d, vec from, vec to)
	{
		bool result = (to.z - (d->o.z - d->eyeheight)) / (d->eyeheight + d->aboveeye) > 0.8f;
		return result;
	}
	bool projdamage(dynent *o, projectile &p, vec &v, int damage)
	{
		if(o->state!=CS_ALIVE) return false;
		if(!intersect(o, p.o, v)) return false;
		projsplash(p, v, o, damage);
		vec dir;
		projdist(o, dir, v);
		hit(damage, o, p.owner, dir, p.gun, 0);
		return true;
	}

	void updateprojectiles(int time)
	{
		loopv(projs)
		{
			projectile &p = projs[i];
			p.offsetmillis = max(p.offsetmillis-time, 0);
			vec dv;
			float dist = p.to.dist(p.o, dv); 
			dv.mul(time/max(dist*1000/p.speed, float(time)));
			vec v = vec(p.o).add(dv);
			bool exploded = false;
			hits.setsize(0);
			if(p.local)
			{
				vec halfdv = vec(dv).mul(0.5f), bo = vec(p.o).add(halfdv);
				float br = max(fabs(halfdv.x), fabs(halfdv.y)) + 1;
				loopj(numdynents())
				{
					dynent *o = iterdynents(j);
					if(p.owner==o || o->o.reject(bo, o->radius + br)) continue;
					if(projdamage(o, p, v, guns[p.gun].damage)) { exploded = true; break; }
				}
			}
			if(!exploded)
			{
				if(dist<4)
				{
					if(p.o!=p.to) // if original target was moving, reevaluate endpoint
					{
						if(raycubepos(p.o, p.dir, p.to, 0, RAY_CLIPMAT|RAY_ALPHAPOLY)>=4) continue;
					}
					projsplash(p, v, NULL, guns[p.gun].damage);
					exploded = true;
				}
				else
				{
					vec pos(v);
					pos.add(vec(p.offset).mul(p.offsetmillis/float(OFFSETMILLIS)));
					if(guns[p.gun].part)
					{
						 regular_particle_splash(PART_SMOKE, 2, 300, pos, 0x404040, 0.6f, 150, -20);
						 int color = 0xFFFFFF;
						 switch(guns[p.gun].part)
						 {
							case PART_FIREBALL1: color = 0xFFC8C8; break;
						 }
						 particle_splash(guns[p.gun].part, 1, 1, pos, color, 4.8f, 150, 20);
					}
					else regular_particle_splash(PART_SMOKE, 2, 300, pos, 0x404040, 2.4f, 50, -20);
				}
			}
			if(exploded)
			{
				if(p.local)
					addmsg(N_EXPLODE, "rci3iv", p.owner, lastmillis-maptime, p.gun, p.id-maptime,
							hits.length(), hits.length()*sizeof(hitmsg)/sizeof(int), hits.getbuf());
				projs.remove(i--);
			}
			else p.o = v;
		}
	}

	extern int chainsawhudgun;

	VARP(muzzleflash, 0, 1, 1);
	VARP(muzzlelight, 0, 1, 1);

	HVARP(rifletrail, 0, 0x404040, 0xFFFFFF);

	VARP(teamcolorrifle, 0, 1, 1);
	
	void shoteffects(int gun, const vec &from, const vec &to, fpsent *d, bool local, int id, int prevaction)     // create visual effect from a shot
	{
		int sound = guns[gun].sound, pspeed = 25;
		switch(gun)
		{
			case GUN_FIST:
				if(d->type==ENT_PLAYER && chainsawhudgun) sound = S_CHAINSAW_ATTACK;
				if((muzzlelight) && (darkmap)) adddynlight(hudgunorigin(gun, d->o, to, d), 25, vec(0.6f, 0.275f, 0.15f), 10, 10, DL_FLASH, 0, vec(0, 0, 0), d);
				break;

			case GUN_SG:
			{
				if(!local) createrays(gun, from, to);
				if(muzzleflash && d->muzzle.x >= 0)
					particle_flare(d->muzzle, d->muzzle, 200, PART_MUZZLE_FLASH3, 0xFFFFFF, 2.75f, d);
				loopi(guns[gun].rays)
				{
					particle_splash(PART_SPARK, 20, 250, rays[i], 0xB49B4B, 0.24f);
					particle_flare(hudgunorigin(gun, from, rays[i], d), rays[i], 300, PART_STREAK, 0xFFC864, 0.28f);
					if(!local) adddecal(DECAL_BULLET, rays[i], vec(from).sub(rays[i]).safenormalize(), 2.0f);
				}
				if(muzzlelight) adddynlight(hudgunorigin(gun, d->o, to, d), 30, vec(0.5f, 0.375f, 0.25f), 100, 100, DL_FLASH, 0, vec(0, 0, 0), d);
				break;
			}

			case GUN_CG:
			case GUN_SMG:
			{
				particle_splash(PART_SPARK, 200, 250, to, 0xB49B4B, 0.24f);
				particle_flare(hudgunorigin(gun, from, to, d), to, 600, PART_STREAK, 0xFFC864, 0.28f);
				if(muzzleflash && d->muzzle.x >= 0)
					particle_flare(d->muzzle, d->muzzle, gun==GUN_CG ? 100 : 200, PART_MUZZLE_FLASH1, 0xFFFFFF, gun==GUN_CG ? 2.25f : 1.25f, d);
				if(!local) adddecal(DECAL_BULLET, to, vec(from).sub(to).safenormalize(), 2.0f);
				if(muzzlelight) adddynlight(hudgunorigin(gun, d->o, to, d), gun==GUN_CG ? 30 : 15, vec(0.5f, 0.375f, 0.25f), gun==GUN_CG ? 50 : 100, gun==GUN_CG ? 50 : 100, DL_FLASH, 0, vec(0, 0, 0), d);
				break;
			}

			case GUN_RL:
				if(muzzleflash && d->muzzle.x >= 0)
					particle_flare(d->muzzle, d->muzzle, 250, PART_MUZZLE_FLASH2, 0xFFFFFF, 3.0f, d);
				pspeed = guns[gun].projspeed;
				if(d->type==ENT_AI) pspeed /= 2;
				newprojectile(from, to, (float)pspeed, local, id, d, gun);
				break;

			case GUN_GL:
			{
				float dist = from.dist(to);
				vec up = to;
				up.z += dist/8;
				if(muzzleflash && d->muzzle.x >= 0)
					particle_flare(d->muzzle, d->muzzle, 200, PART_MUZZLE_FLASH2, 0xFFFFFF, 1.5f, d);
				if(muzzlelight) adddynlight(hudgunorigin(gun, d->o, to, d), 20, vec(0.5f, 0.375f, 0.25f), 100, 100, DL_FLASH, 0, vec(0, 0, 0), d);
				newbouncer(from, up, local, id, d, BNC_GRENADE, guns[gun].ttl, guns[gun].projspeed);
				break;
			}

			case GUN_RIFLE:
				particle_splash(PART_SPARK, 200, 250, to, 0xB49B4B, 0.24f);
				
				int rcolor = !strcmp(d->team, "red") ? 0xFF0000 : 0x0000FF;
				if(!m_teammode || !teamcolorrifle) rcolor = rifletrail;
				particle_trail(PART_SMOKE, 500, hudgunorigin(gun, from, to, d), to, rcolor, 0.6f, 20);

				if(muzzleflash && d->muzzle.x >= 0)
					particle_flare(d->muzzle, d->muzzle, 150, PART_MUZZLE_FLASH3, 0xFFFFFF, 1.25f, d);
				if(!local) adddecal(DECAL_BULLET, to, vec(from).sub(to).safenormalize(), 3.0f);
				if(muzzlelight) adddynlight(hudgunorigin(gun, d->o, to, d), 25, vec(0.5f, 0.375f, 0.25f), 75, 75, DL_FLASH, 0, vec(0, 0, 0), d);
				break;
		}

		bool looped = false;
		if(d->attacksound >= 0 && d->attacksound != sound) d->stopattacksound();
		if(d->idlesound >= 0) d->stopidlesound();
		fpsent *h = followingplayer(player1);
		switch(sound)
		{
			case S_CHAINSAW_ATTACK:
				if(d->attacksound >= 0) looped = true;
				d->attacksound = sound;
				d->attackchan = playsound(sound, d==h ? NULL : &d->o, NULL, 0, -1, 100, d->attackchan);
				break;
			default:
				playsound(sound, d==h ? NULL : &d->o);
				break;
		}
	}

	void particletrack(physent *owner, vec &o, vec &d)
	{
		if(owner->type!=ENT_PLAYER && owner->type!=ENT_AI) return;
		fpsent *pl = (fpsent *)owner;
		if(pl->muzzle.x < 0 || pl->lastattackgun != pl->gunselect) return;
		float dist = o.dist(d);
		o = pl->muzzle;
		if(dist <= 0) d = o;
		else
		{
			vecfromyawpitch(owner->yaw, owner->pitch, 1, 0, d);
			float newdist = raycube(owner->o, d, dist, RAY_CLIPMAT|RAY_ALPHAPOLY);
			d.mul(min(newdist, dist)).add(owner->o);
		}
	}

	void dynlighttrack(physent *owner, vec &o, vec &hud)
	{
		if(owner->type!=ENT_PLAYER && owner->type!=ENT_AI) return;
		fpsent *pl = (fpsent *)owner;
		if(pl->muzzle.x < 0 || pl->lastattackgun != pl->gunselect) return;
		o = pl->muzzle;
		hud = owner == followingplayer(player1) ? vec(pl->o).add(vec(0, 0, 2)) : pl->muzzle;
	}

	float intersectdist = 1e16f;

	bool intersect(dynent *d, const vec &from, const vec &to, float &dist)   // if lineseg hits entity bounding box
	{
		vec bottom(d->o), top(d->o);
		bottom.z -= d->eyeheight;
		top.z += d->aboveeye;
		return linecylinderintersect(from, to, bottom, top, d->radius, dist);
	}

	dynent *intersectclosest(const vec &from, const vec &to, fpsent *at, float &bestdist)
	{
		dynent *best = NULL;
		bestdist = 1e16f;
		loopi(numdynents())
		{
			dynent *o = iterdynents(i);
			if(o==at || o->state!=CS_ALIVE) continue;
			float dist;
			if(!intersect(o, from, to, dist)) continue;
			if(dist<bestdist)
			{
				best = o;
				bestdist = dist;
			}
		}
		return best;
	}

	void shorten(vec &from, vec &target, float dist)
	{
		target.sub(from).mul(min(1.0f, dist)).add(from);
	}

	void raydamage(vec &from, vec &to, fpsent *d)
	{
		int qdam = guns[d->gunselect].damage;
		dynent *o;
		float dist;
		if(guns[d->gunselect].rays > 1)
		{
			dynent *hits[MAXRAYS];
			int maxrays = guns[d->gunselect].rays;
			loopi(maxrays) 
			{
				if((hits[i] = intersectclosest(from, rays[i], d, dist))) shorten(from, rays[i], dist);
				else adddecal(DECAL_BULLET, rays[i], vec(from).sub(rays[i]).safenormalize(), 2.0f);
			}
			loopi(maxrays) if(hits[i])
			{
				o = hits[i];
				hits[i] = NULL;
				int numhits = 1;
				for(int j = i+1; j < maxrays; j++) if(hits[j] == o)
				{
					hits[j] = NULL;
					numhits++;
				}
				hitpush(numhits*qdam, o, d, from, to, d->gunselect, numhits, false);
			}
		}
		else if((o = intersectclosest(from, to, d, dist)))
		{
			shorten(from, to, dist);
			int headshot = isheadshot(o, from, to);
			hitpush(qdam, o, d, from, to, d->gunselect, 1, headshot);
		}
		else if(d->gunselect!=GUN_FIST) adddecal(DECAL_BULLET, to, vec(from).sub(to).safenormalize(), d->gunselect==GUN_RIFLE ? 3.0f : 2.0f);
	}

	void shoot(fpsent *d, const vec &targ, bool secondary)
	{
		int prevaction = d->lastaction[d->gunselect], attacktime = lastmillis-prevaction;
		if(attacktime<d->gunwait[d->gunselect]) return;
		d->gunwait[d->gunselect] = 0;
		if((d==player1 || d->ai) && (!d->attacking && !d->secattacking)) return;
		d->lastaction[d->gunselect] = lastmillis;
		d->lastattackgun = d->gunselect;
		if(!d->ammo[d->gunselect])
		{
			if(d==player1)
			{
				msgsound(S_NOAMMO, d);
				d->gunwait[d->gunselect] = 600;
				d->lastattackgun = -1;
				weaponswitch(d);
			}
			return;
		}
		if(d->gunselect && !m_bottomless) d->ammo[d->gunselect]--; // subtract ammo

		vec from = d->o, to = targ, dir = vec(to).sub(from).safenormalize();
		float dist = to.dist(from);
		vec kickback = vec(dir).mul(guns[d->gunselect].kickamount*-2.5f);
		d->vel.add(kickback);
		float shorten = 0;
		if(guns[d->gunselect].range && dist > guns[d->gunselect].range)
			shorten = guns[d->gunselect].range;
		float barrier = raycube(d->o, dir, dist, RAY_CLIPMAT|RAY_ALPHAPOLY);
		if(barrier > 0 && barrier < dist && (!shorten || barrier < shorten))
			shorten = barrier;
		if(shorten) to = vec(dir).mul(shorten).add(from);

		if(guns[d->gunselect].rays > 1) createrays(d->gunselect, from, to);
		else if(guns[d->gunselect].spread) offsetray(from, to, guns[d->gunselect].spread, guns[d->gunselect].range, to);

		hits.setsize(0);

		if(!guns[d->gunselect].projspeed) raydamage(from, to, d);

		shoteffects(d->gunselect, from, to, d, true, 0, prevaction);

		d->gunwait[d->gunselect] = guns[d->gunselect].attackdelay;

		if(d==player1 || d->ai)
		{
			addmsg(N_SHOOT, "rci2i6iv", d, lastmillis-maptime, d->gunselect,
				   (int)(from.x*DMF), (int)(from.y*DMF), (int)(from.z*DMF),
				   (int)(to.x*DMF), (int)(to.y*DMF), (int)(to.z*DMF),
				   hits.length(), hits.length()*sizeof(hitmsg)/sizeof(int), hits.getbuf());
		}

		//if(d->ai) d->gunwait[d->gunselect] += int(d->gunwait[d->gunselect]*(((101-d->skill)+rnd(111-d->skill))/100.f));
		d->totalshots += guns[d->gunselect].damage*guns[d->gunselect].rays;
		recordpotentialdamage(d);

		if(!d->ammo[d->gunselect] && d==player1)
		{
			msgsound(S_NOAMMO, d);
			d->gunwait[d->gunselect] = 600;
			d->lastattackgun = -1;
			weaponswitch(d);
		}
	}

	void adddynlights()
	{
		loopv(projs)
		{
			projectile &p = projs[i];
			if(p.gun!=GUN_RL) continue;
			vec pos(p.o);
			pos.add(vec(p.offset).mul(p.offsetmillis/float(OFFSETMILLIS)));
			adddynlight(pos, 20, vec(1, 0.75f, 0.5f));
		}
		loopv(bouncers)
		{
			bouncer &bnc = *bouncers[i];
			if(bnc.bouncetype!=BNC_GRENADE) continue;
			vec pos = bnc.offsetpos();
			vec grenclr = (m_teammode ? (!strcmp(bnc.owner->team, "red") ? vec(1, 0.25f, 0.25f) : vec(0.25f, 0.25f, 1)) : vec(0.8f, 0.25f, 1));
			// we're going to pretend this isn't gross. -Y
			adddynlight(pos, (float)(bnc.lifetime/100.0f)+1.0f, grenclr);
		}
	}

	static const char * const projnames[4] = { "projectiles/grenade", "projectiles/grenade/red", "projectiles/grenade/blue", "projectiles/rocket" };
	static const char * const gibnames[3] = { "gibs/gib01", "gibs/gib02", "gibs/gib03" };
	static const char * const debrisnames[4] = { "debris/debris01", "debris/debris02", "debris/debris03", "debris/debris04" };

	void preloadbouncers()
	{
		loopi(sizeof(projnames)/sizeof(projnames[0])) preloadmodel(projnames[i]);
		loopi(sizeof(gibnames)/sizeof(gibnames[0])) preloadmodel(gibnames[i]);
		loopi(sizeof(debrisnames)/sizeof(debrisnames[0])) preloadmodel(debrisnames[i]);
	}

	void renderbouncers()
	{
		float yaw, pitch;
		loopv(bouncers)
		{
			bouncer &bnc = *bouncers[i];
			vec pos = bnc.offsetpos();
			vec vel(bnc.vel);
			if(vel.magnitude() <= 25.0f) yaw = bnc.lastyaw;
			else
			{
				vectoyawpitch(vel, yaw, pitch);
				yaw += 90;
				bnc.lastyaw = yaw;
			}
			pitch = -bnc.roll;
			if (bnc.bouncetype==BNC_GRENADE)
			{
				//conoutf(CON_DEBUG, "%s (team %s), rendering grenade", bnc.owner->name, bnc.owner->team);
				cbstring grenmdl = "projectiles/grenade";
				if(m_teammode) formatstring(grenmdl, "projectiles/grenade/%s", bnc.owner->team);
				rendermodel(&bnc.light, grenmdl, ANIM_MAPMODEL|ANIM_LOOP, pos, yaw, pitch, MDL_CULL_VFC|MDL_CULL_OCCLUDED|MDL_LIGHT|MDL_LIGHT_FAST|MDL_DYNSHADOW);
			}
			else
			{
				const char *mdl = NULL;
				int cull = MDL_CULL_VFC|MDL_CULL_DIST|MDL_CULL_OCCLUDED;
				float fade = 1;
				if(bnc.lifetime < 250) fade = bnc.lifetime/250.0f;
				switch(bnc.bouncetype)
				{
					case BNC_GIBS: mdl = gibnames[bnc.variant]; cull |= MDL_LIGHT|MDL_LIGHT_FAST|MDL_DYNSHADOW; break;
					case BNC_DEBRIS: mdl = debrisnames[bnc.variant]; break;
					default: continue;
				}
				rendermodel(&bnc.light, mdl, ANIM_MAPMODEL|ANIM_LOOP, pos, yaw, pitch, cull, NULL, NULL, 0, 0, fade);
			}
		}
	}

	void renderprojectiles()
	{
		float yaw, pitch;
		loopv(projs)
		{
			projectile &p = projs[i];
			if(p.gun!=GUN_RL) continue;
			float dist = min(p.o.dist(p.to)/32.0f, 1.0f);
			vec pos = vec(p.o).add(vec(p.offset).mul(dist*p.offsetmillis/float(OFFSETMILLIS))),
				v = dist < 1e-6f ? p.dir : vec(p.to).sub(pos).normalize();
			// the amount of distance in front of the smoke trail needs to change if the model does
			vectoyawpitch(v, yaw, pitch);
			yaw += 90;
			v.mul(3);
			v.add(pos);
			rendermodel(&p.light, "projectiles/rocket", ANIM_MAPMODEL|ANIM_LOOP, v, yaw, pitch, MDL_CULL_VFC|MDL_CULL_OCCLUDED|MDL_LIGHT|MDL_LIGHT_FAST);
		}
	}

	void checkattacksound(fpsent *d, bool local)
	{
		int gun = -1;
		switch(d->attacksound)
		{
			case S_CHAINSAW_ATTACK:
				if(chainsawhudgun) gun = GUN_FIST;
				break;
			default:
				return;
		}
		if(gun >= 0 && gun < NUMGUNS &&
		   d->clientnum >= 0 && d->state == CS_ALIVE &&
		   d->lastattackgun == gun && lastmillis - d->lastaction[d->gunselect] < guns[gun].attackdelay + 50)
		{
			d->attackchan = playsound(d->attacksound, local ? NULL : &d->o, NULL, 0, -1, -1, d->attackchan);
			if(d->attackchan < 0) d->attacksound = -1;
		}
		else d->stopattacksound();
	}

	void checkidlesound(fpsent *d, bool local)
	{
		int sound = -1, radius = 0;
		if(d->clientnum >= 0 && d->state == CS_ALIVE) switch(d->gunselect)
		{
			case GUN_FIST:
				if(chainsawhudgun && d->attacksound < 0)
				{
					sound = S_CHAINSAW_IDLE;
					radius = 50;
				}
				break;
		}
		if(d->idlesound != sound)
		{
			if(d->idlesound >= 0) d->stopidlesound();
			if(sound >= 0)
			{
				d->idlechan = playsound(sound, local ? NULL : &d->o, NULL, 0, -1, 100, d->idlechan, radius);
				if(d->idlechan >= 0) d->idlesound = sound;
			}
		}
		else if(sound >= 0)
		{
			d->idlechan = playsound(sound, local ? NULL : &d->o, NULL, 0, -1, -1, d->idlechan, radius);
			if(d->idlechan < 0) d->idlesound = -1;
		}
	}

	void removeweapons(fpsent *d)
	{
		removebouncers(d);
		removeprojectiles(d);
	}

	void updateweapons(int curtime)
	{
		movegrapples(curtime);
		if (player1->clientnum >= 0 && player1->state == CS_ALIVE) // only shoot when connected to server
		{
			shoot(player1, worldpos, false);
			shootgrapple(player1, worldpos);
		}

		updateprojectiles(curtime);
		if(player1->clientnum>=0 && player1->state==CS_ALIVE) shoot(player1, worldpos, false); // only shoot when connected to server
		updatebouncers(curtime); // need to do this after the player shoots so grenades don't end up inside player's BB next frame
		fpsent *following = followingplayer();
		if(!following) following = player1;
		loopv(players)
		{
			fpsent *d = players[i];
			checkattacksound(d, d==following);
			checkidlesound(d, d==following);
		}
	}

	void avoidweapons(ai::avoidset &obstacles, float radius)
	{
		loopv(projs)
		{
			projectile &p = projs[i];
			obstacles.avoidnear(NULL, p.o.z + guns[p.gun].exprad + 1, p.o, radius + guns[p.gun].exprad);
		}
		loopv(bouncers)
		{
			bouncer &bnc = *bouncers[i];
			if(bnc.bouncetype != BNC_GRENADE) continue;
			obstacles.avoidnear(NULL, bnc.o.z + guns[GUN_GL].exprad + 1, bnc.o, radius + guns[GUN_GL].exprad);
		}
	}
	
	struct grappleobj
	{
		vec dir, o, to, offset;
		float speed;
		fpsent* owner;
		bool attached;
		physent* attachedto;
		bool local;
		int offsetmillis;
		int id;
		entitylight light;
	};
	vector<grappleobj> grapples;

	void reset() { grapples.setsize(0); player1->grappling = false; }

	grappleobj& newgrapple(vec& from, vec& to, float speed, bool local, fpsent* owner)
	{
		grappleobj& g = grapples.add();
		g.dir = vec(to).sub(from).normalize();
		g.o = from;
		g.to = to;
		g.offset = hudgunorigin(0, from, to, owner);
		g.offset.sub(from);
		g.speed = speed;
		g.local = local;
		g.owner = owner;
		g.attached = false;
		g.attachedto = NULL;
		g.offsetmillis = OFFSETMILLIS;
		g.id = lastmillis;
		return g;
	}

	bool hasgrapple(fpsent* d)
	{
		loopv(grapples)
			if (grapples[i].owner == d) return true;
		return false;
	}

	void removegrapples(physent* d, bool attached)
	{
		// can't use loopv here due to strange GCC optimizer bug
		int len = grapples.length();
		for (int i = 0; i < len; i++) if (grapples[i].owner == d || (attached && grapples[i].attachedto == d))
		{
			grapples.remove(i--);
			len--;
		}
		if (d == player1)
		{
			addmsg(N_GRAPPLESTOP, "ri", (int)attached);
			player1->grappling = false;
		}
	}

	void setgrapplepos(fpsent* d, const vec& pos)
	{
		loopv(grapples) if (grapples[i].owner == d)
		{
			grapples[i].o = pos;
			break;
		}
	}

	void setgrapplehit(fpsent* d, const vec& hit)
	{
		loopv(grapples) if (grapples[i].owner == d)
		{
			grapples[i].o = hit;
			grapples[i].to = hit;
			grapples[i].attached = true;
			break;
		}
	}

	void setgrappled(fpsent* d, fpsent* victim)
	{
		loopv(grapples) if (grapples[i].owner == d)
		{
			grapples[i].attachedto = victim;
			grapples[i].attached = true;
			break;
		}
	}

	void sendgrappleclient(dynent* d, ucharbuf& p)
	{
		loopv(grapples)
		{
			grappleobj& g = grapples[i];
			if (grapples[i].owner == d && !g.attached)
			{
				putint(p, N_GRAPPLEPOS);
				for (int k = 0; k < 3; k++) putint(p, int(g.o[k] * DMF));
				return;
			}
		}
	}

	bool grappleent(dynent* o, grappleobj& g, vec& v)
	{
		if (o->state != CS_ALIVE) return false;
		if (!intersect(o, g.o, v)) return false;
		return true;
	}

	void movegrapples(int time)
	{
		loopv(grapples)
		{
			grappleobj& g = grapples[i];
			fpsent* d = g.owner;
			if (!g.attached)
			{
				g.offsetmillis = max(g.offsetmillis - time, 0);
				vec v;
				float dist = g.to.dist(g.o, v);
				float dtime = dist * 1000 / g.speed;
				if (time > dtime) dtime = time;
				v.mul(time / dtime);
				v.add(g.o);
				for (int j=0; j<numdynents(); j++)
				{
					dynent* o = iterdynents(j);
					if (!o || d == o || o->o.reject(v, 10.0f)) continue;
					if (grappleent(o, g, v))
					{
						g.attachedto = o;
						g.attached = true;
						break;
					}
				}
				if (!g.attached)
				{
					if (dist < 4)
					{
						if (g.o != g.to) // if original target was moving, reevaluate endpoint
						{
							if (raycubepos(g.o, g.dir, g.to, 0, RAY_CLIPMAT | RAY_ALPHAPOLY) >= 4) continue;
						}
						g.attached = true;
					}
				}
				if (!g.attached) g.o = v;
				else if (d == player1)
				{
					physent* o = g.attachedto;
					if (o != NULL && o->type == ENT_PLAYER)
					{
						addmsg(N_GRAPPLED, "ri", ((fpsent*)o)->clientnum);
					}
					else
					{
						addmsg(N_GRAPPLEHIT, "ri3", int(g.o.x * DMF), int(g.o.y * DMF), int(g.o.z * DMF));
					}
				}
			}
			else
			{
				vec to(g.attachedto ? g.attachedto->o : g.to);
				vec dir(to);
				dir.sub(d->o);
				if (dir.squaredlen() > GRAPPLESEPERATION * GRAPPLESEPERATION)
				{
					dir.normalize();
					if (g.attachedto)
					{
						physent* a = g.attachedto;
						dir.mul(GRAPPLEPULLSPEED * 0.5);
						a->vel.sub(dir);
					}
					else
					{
						dir.mul(GRAPPLEPULLSPEED);
						g.owner->vel.add(dir);
					}
				}
			}
		}
	}

	void shootgrapplev(vec& from, vec& to, fpsent* d, bool local)
	{
		newgrapple(from, to, GRAPPLETHROWSPEED, local, d);
	}

	void shootgrapple(fpsent* d, vec& targ)
	{
		if (d == player1 && !d->grappling) return;
		if (hasgrapple(d)) return;

		vec from = d->o;
		vec to = targ;

		vec unitv;
		float dist = to.dist(from, unitv);
		unitv.div(dist);
		float barrier = raycube(d->o, unitv, dist, RAY_CLIPMAT | RAY_ALPHAPOLY);
		if (barrier < dist)
		{
			to = unitv;
			to.mul(barrier);
			to.add(from);
		}

		shootgrapplev(from, to, d, true);

		if (d == player1)
		{
			addmsg(N_GRAPPLE, "rii6", lastmillis - maptime,
				(int)(from.x * DMF), (int)(from.y * DMF), (int)(from.z * DMF),
				(int)(to.x * DMF), (int)(to.y * DMF), (int)(to.z * DMF));
		}
	}

	void rendergrapples()
	{
		float yaw, pitch;
		loopv(grapples)
		{
			grappleobj& g = grapples[i];
			vec pos(g.o), to(g.to);
			if (g.attachedto)
			{
				to = g.attachedto->o;
				to.z += (g.attachedto->aboveeye - g.attachedto->eyeheight) / 2;
			}
			pos.add(vec(g.offset).mul(g.offsetmillis / float(OFFSETMILLIS)));
			vec v(to);
			if (g.attached || pos == g.to)
			{
				v.sub(g.owner->o);
				v.normalize();
				vectoyawpitch(v, yaw, pitch);
				v = to;
			}
			else
			{
				v.sub(pos);
				v.normalize();
				vectoyawpitch(v, yaw, pitch);
				yaw += 45;
				v.mul(3);
				v.add(pos);
			}
			rendermodel(&g.light, "grapple", ANIM_MAPMODEL | ANIM_LOOP, v, yaw, pitch, MDL_CULL_VFC | MDL_CULL_OCCLUDED | MDL_LIGHT);
			particle_trail(PART_CHAIN, 1, hudgunorigin(0, g.owner->o, v, g.owner), v, 0xFF0000);
		}
	}
};

