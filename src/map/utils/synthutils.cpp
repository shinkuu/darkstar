﻿/*
===========================================================================

  Copyright (c) 2010-2012 Darkstar Dev Teams

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

  This file is part of DarkStar-server source code.

===========================================================================
*/

#include "../../common/socket.h"
#include "../../common/showmsg.h"
#include "../../common/utils.h"

#include <math.h>
#include <string.h> 

#include "../packets/char_skills.h"
#include "../packets/char_update.h"
#include "../packets/inventory_assign.h"
#include "../packets/inventory_finish.h"
#include "../packets/inventory_item.h"
#include "../packets/message_basic.h"
#include "../packets/synth_animation.h"
#include "../packets/synth_message.h"

#include "charutils.h"
#include "itemutils.h"
#include "../map.h"
#include "synthutils.h"
#include "../vana_time.h"
#include "zoneutils.h"

//#define _DSP_SYNTH_DEBUG_MESSAGES_ // включаем отладочные сообщения

enum SYNTHESIS_ELEMENT
{
	ELEMENT_FIRE		= 0,
	ELEMENT_EARTH		= 1,
	ELEMENT_WATER		= 2,
	ELEMENT_WIND		= 3,
	ELEMENT_ICE			= 4,
	ELEMENT_LIGHTNING	= 5,
	ELEMENT_LIGHT		= 6,
	ELEMENT_DARK		= 7
};

enum SYNTHESIS_RESULT
{
	SYNTHESIS_FAIL		= 0,
	SYNTHESIS_SUCCESS	= 1,
	SYNTHESIS_HQ		= 2,
	SYNTHESIS_HQ2		= 3,
	SYNTHESIS_HQ3		= 4
};


namespace synthutils
{

/************************************************************************
*																		*
*  Проверяем наличие рецепта и возможности его синтеза (если его		*
*  сложность выше на 15 уровней, чем умение персонажа, то рецепт		* 
*  считается сверх трудным и синтех отменяется). Так же собираем всю	*
*  необходимую информацию о рецепте, чтобы не обращаться к базе			*
*  несколько раз														*
*																		*		
*  в поля itemID девятой ячейки сохраняем ID рецепта					*
*  в поля quantity 9-16 ячеек записываем требуемые значения skills		*
*  в поля itemID и slotID 10-14 ячеек записываем результаты синтеза		*
*																		*
************************************************************************/

bool isRightRecipe(CCharEntity* PChar) 
{
	const int8* fmtQuery = 

		"SELECT ID, KeyItem, Wood, Smith, Gold, Cloth, Leather, Bone, Alchemy, Cook, \
			Result, ResultHQ1, ResultHQ2, ResultHQ3, ResultQty, ResultHQ1Qty, ResultHQ2Qty, ResultHQ3Qty \
		FROM synth_recipes \
		WHERE (Crystal = %u OR HQCrystal = %u) \
			AND Ingredient1 = %u \
			AND Ingredient2 = %u \
			AND Ingredient3 = %u \
			AND Ingredient4 = %u \
			AND Ingredient5 = %u \
			AND Ingredient6 = %u \
			AND Ingredient7 = %u \
			AND Ingredient8 = %u \
		LIMIT 1";

	int32 ret = Sql_Query(
		SqlHandle,
		fmtQuery,
		PChar->Container->getItemID(0), 
		PChar->Container->getItemID(0),
		PChar->Container->getItemID(1), 
		PChar->Container->getItemID(2), 
		PChar->Container->getItemID(3), 
		PChar->Container->getItemID(4), 
		PChar->Container->getItemID(5), 
		PChar->Container->getItemID(6), 
		PChar->Container->getItemID(7), 
		PChar->Container->getItemID(8));

	if (ret != SQL_ERROR && 
		Sql_NumRows(SqlHandle) != 0 &&
		Sql_NextRow(SqlHandle) == SQL_SUCCESS)
	{
		uint16 KeyItemID = (uint16)Sql_GetUIntData(SqlHandle,1);

		if ((KeyItemID == 0) || (charutils::hasKeyItem(PChar,KeyItemID)))
		{
			// в девятую ячейку записываем id рецепта
			PChar->Container->setItem(9, Sql_GetUIntData(SqlHandle,0),0xFF,0);
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Recipe matches ID %u.\n" CL_RESET, PChar->Container->getItemID(9));
			#endif

			PChar->Container->setItem(10 + 1, (uint16)Sql_GetUIntData(SqlHandle,10), (uint8)Sql_GetUIntData(SqlHandle,14), 0);	// RESULT_SUCCESS
			PChar->Container->setItem(10 + 2, (uint16)Sql_GetUIntData(SqlHandle,11), (uint8)Sql_GetUIntData(SqlHandle,15), 0);	// RESULT_HQ
			PChar->Container->setItem(10 + 3, (uint16)Sql_GetUIntData(SqlHandle,12), (uint8)Sql_GetUIntData(SqlHandle,16), 0);	// RESULT_HQ2
			PChar->Container->setItem(10 + 4, (uint16)Sql_GetUIntData(SqlHandle,13), (uint8)Sql_GetUIntData(SqlHandle,17), 0);	// RESULT_HQ3

			uint16 skillValue   = 0;
			uint16 currentSkill = 0; 

			for (uint8 skillID = 49; skillID < 57; ++skillID)
			{
				skillValue   = (uint16)Sql_GetUIntData(SqlHandle,(skillID-49+2));	
				currentSkill = PChar->RealSkills.skill[skillID];

				// skill записываем в поле quantity ячеек 9-16
				PChar->Container->setQuantity(skillID-40, skillValue);
				
				#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
				ShowDebug(CL_CYAN"Current skill = %u, Recipe skill = %u\n" CL_RESET, currentSkill, skillValue*10);
				#endif
				if (currentSkill < (skillValue*10 - 150))
				{
					PChar->pushPacket(new CSynthMessagePacket(PChar, SYNTH_NOSKILL));
					#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
					ShowDebug(CL_CYAN"Not enough skill. Synth aborted.\n" CL_RESET);
					#endif
					return false;
				}
			}
			return true;
		}
	}

	PChar->pushPacket(new CSynthMessagePacket(PChar, SYNTH_BADRECIPE));
	#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
	ShowDebug(CL_CYAN"Recipe not found. Synth aborted.\n" CL_RESET);
	ShowDebug(CL_CYAN"Ingredient1 = %u\n" CL_RESET, PChar->Container->getItemID(1));
	ShowDebug(CL_CYAN"Ingredient2 = %u\n" CL_RESET, PChar->Container->getItemID(2));
	ShowDebug(CL_CYAN"Ingredient3 = %u\n" CL_RESET, PChar->Container->getItemID(3));
	ShowDebug(CL_CYAN"Ingredient4 = %u\n" CL_RESET, PChar->Container->getItemID(4));
	ShowDebug(CL_CYAN"Ingredient5 = %u\n" CL_RESET, PChar->Container->getItemID(5));
	ShowDebug(CL_CYAN"Ingredient6 = %u\n" CL_RESET, PChar->Container->getItemID(6));
	ShowDebug(CL_CYAN"Ingredient7 = %u\n" CL_RESET, PChar->Container->getItemID(7));
	ShowDebug(CL_CYAN"Ingredient8 = %u\n" CL_RESET, PChar->Container->getItemID(8));
	#endif
	return false;
}

/************************************************************************
*																		*
*  Расчитываем сложность синтеза для конкретного умения. Хорошо бы		*
*  сохранять результат в какой-нибудь ячейке контейнера (тип dooble)	*
*																		*
************************************************************************/

double getSynthDifficulty(CCharEntity* PChar, uint8 skillID)
{
	uint8  ElementDirection = 0;
	uint8  WeekDay = (uint8)CVanaTime::getInstance()->getWeekday();
	uint8  crystalElement = PChar->Container->getType();
	uint8  direction = (PChar->loc.p.rotation - 16)/32;
	uint8  strongElement[8] = {2,3,5,4,0,1,7,6};
	uint16 ModID = 0; 
	
	switch (direction)
	{
		case 0: ElementDirection = ELEMENT_WIND;	  break;
		case 1: ElementDirection = ELEMENT_EARTH;	  break;
		case 2: ElementDirection = ELEMENT_LIGHTNING; break;
		case 3: ElementDirection = ELEMENT_WATER;	  break;
		case 4: ElementDirection = ELEMENT_FIRE;	  break;
		case 5: ElementDirection = ELEMENT_DARK;	  break;
		case 6: ElementDirection = ELEMENT_LIGHT;	  break;
		case 7: ElementDirection = ELEMENT_ICE;		  break;
	}

	switch (skillID)
	{
		case SKILL_WDW: ModID = MOD_WOOD;		break;
		case SKILL_SMT: ModID = MOD_SMITH;		break;
		case SKILL_GLD: ModID = MOD_GOLDSMITH;	break;
		case SKILL_CLT: ModID = MOD_CLOTH;		break;
		case SKILL_LTH: ModID = MOD_LEATHER;	break;
		case SKILL_BON: ModID = MOD_BONE;		break;
		case SKILL_ALC: ModID = MOD_ALCHEMY;	break;
		case SKILL_COK: ModID = MOD_COOK;		break;
	}

	double charSkill = (double)(PChar->RealSkills.skill[skillID])/10;
	double difficult = PChar->Container->getQuantity(skillID-40) - (charSkill + PChar->getMod(ModID));
	double MoonPhase = (double)CVanaTime::getInstance()->getMoonPhase();

	difficult -= (abs(MoonPhase - 50))/50;

	if (crystalElement == ElementDirection){
		difficult -= 0.5;
	}else if (strongElement[crystalElement] == ElementDirection){
		difficult += 0.5;
	}

	if (crystalElement == WeekDay){
		difficult -= 1;
	}else if (strongElement[crystalElement] == WeekDay){
		difficult += 1;
	}else if (WeekDay == LIGHTSDAY){
		difficult -= 1;
	}else if (WeekDay == DARKSDAY){
		difficult += 1;	
	}

	#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
	ShowDebug(CL_CYAN"Direction = %i\n" CL_RESET, ElementDirection);
	ShowDebug(CL_CYAN"Day = %i\n" CL_RESET, WeekDay);
	ShowDebug(CL_CYAN"Moon = %g\n" CL_RESET, MoonPhase);
	ShowDebug(CL_CYAN"Difficulty = %g\n" CL_RESET, difficult);
	#endif
	
	return difficult;
}

/************************************************************************
*																		*
*  Проверяем возможность создания предметов высокого качества			*
*  Это сделано из-за наличия в игре специфических колец. 				*
*																		*
************************************************************************/

bool canSynthesizeHQ(CCharEntity* PChar, uint8 skillID)
{
	uint16 ModID = 0; 

	switch (skillID)
	{
		case SKILL_WDW: ModID = MOD_ANTIHQ_WOOD;	  break;
		case SKILL_SMT: ModID = MOD_ANTIHQ_SMITH;	  break;
		case SKILL_GLD: ModID = MOD_ANTIHQ_GOLDSMITH; break;
		case SKILL_CLT: ModID = MOD_ANTIHQ_CLOTH;	  break;
		case SKILL_LTH: ModID = MOD_ANTIHQ_LEATHER;	  break;
		case SKILL_BON: ModID = MOD_ANTIHQ_BONE;	  break;
		case SKILL_ALC: ModID = MOD_ANTIHQ_ALCHEMY;	  break;
		case SKILL_COK: ModID = MOD_ANTIHQ_COOK;	  break;
	}

	return (PChar->getMod(ModID) != 0 ? false : true);
}

/************************************************************************
*																		*
*  Получаем ID главного умения в рецепте. Именно от него зависит		*
*  возможность создания предметов высокого качества						*
*																		*
************************************************************************/

uint8 getGeneralCraft(CCharEntity* PChar)
{
	uint8 skillValue   = 0;
	uint8 generalCraft = 0;
	
	for(uint8 skillID = 49; skillID < 57; skillID ++) 
	{
		if (PChar->Container->getQuantity(skillID-40) > skillValue)
		{
			skillValue = PChar->Container->getQuantity(skillID-40);
			generalCraft = skillID;
		}
	}

	return generalCraft;
}

/************************************************************************
*																		*
*  Расчет результата синтеза.											*
*																		*
*  результат синтеза записываем в поле quantity ячейки кристалла.		*
*  сохраняем в slotID ячейки кристалла ID умения, из-за котороги синтез *
*  провалился.															*
*																		*
************************************************************************/

uint8 calcSynthResult(CCharEntity* PChar) 
{
	uint8 count  = 0;
	uint8 result = 0;
	uint8 hqtier = 0;

	double success = 0;
	double chance  = 0;

	for(uint8 skillID = 49; skillID < 57; ++skillID) 
	{
		uint8 checkSkill = PChar->Container->getQuantity(skillID-40);
		if(checkSkill != 0) 
		{
			double synthDiff = getSynthDifficulty(PChar, skillID);
			hqtier = 0;
			count++;

			if(synthDiff <= 0) 
			{
				success = 0.95;

				if((synthDiff <= 0) && (synthDiff >= -10)){
					success -= (double)(PChar->Container->getType() == ELEMENT_LIGHTNING) * 0.2;
					hqtier = 1;
				}else if((synthDiff <= -11) && (synthDiff >= -30)){
					hqtier = 2;
				}else if((synthDiff <= -31) && (synthDiff >= -50)){
					hqtier = 3;
				}else if((synthDiff <= -51) && (synthDiff >= -70)){
					hqtier = 4;
				}else if (synthDiff <= -71)
					hqtier = 5;
			}else{
				success = 0.95 - (synthDiff / 10) - (double)(PChar->Container->getType() == ELEMENT_LIGHTNING) * 0.2;			
				if(success < 0.05)
					success = 0.05;
			}

			double random = rand() / ((double) RAND_MAX);
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Success: %g  Random: %g\n" CL_RESET, success, random);
			#endif

			if(random < success) 
			{
				for(int32 i = 0; i < 3; ++i) 
				{
					random = rand() / ((double) RAND_MAX);
					#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
					ShowDebug(CL_CYAN"HQ Tier: %i  Random: %g\n" CL_RESET, hqtier, random);
					#endif
					
					switch(hqtier) 
					{
						case 5:  chance = 0.700; break;
						case 4:  chance = 0.500; break;
						case 3:  chance = 0.300; break;
						case 2:  chance = 0.100; break;
						case 1:  chance = 0.015; break;
						default: chance = 0.000; break;
					}
					if(chance < random)
						break;
					result += 1;
					hqtier -= 1;
				}
			}else{
				// сохраняем умение, из-за которого синтез провалился.
				// используем slotID ячейки кристалла, т.к. он был удален еще в начале синтеза
				PChar->Container->setInvSlotID(0,skillID);
				result = -1;
				break;
			}
		}
	}

	result = (count == 0 ? SYNTHESIS_SUCCESS : (result == 0xFF ? SYNTHESIS_FAIL : (result/count+1)));

	if ((result > SYNTHESIS_SUCCESS) && (!canSynthesizeHQ(PChar,getGeneralCraft(PChar))))
	{
		result = SYNTHESIS_SUCCESS;
	}

	// результат синтеза записываем в поле quantity ячейки кристалла.
	PChar->Container->setQuantity(0, result);

	switch(result)
	{
		case SYNTHESIS_FAIL:
			result = RESULT_FAIL;
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Synth failed.\n" CL_RESET);
			#endif
			break;
		case SYNTHESIS_SUCCESS:
			result = RESULT_SUCCESS;
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Synth success.\n" CL_RESET);
			#endif
			break;
		case SYNTHESIS_HQ:
			result = RESULT_HQ;
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Synth HQ.\n" CL_RESET);
			#endif
			break;
		case SYNTHESIS_HQ2:
			result = RESULT_HQ;
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Synth HQ2.\n" CL_RESET);
			#endif
			break;
		case SYNTHESIS_HQ3:
			result = RESULT_HQ;
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Synth HQ3.\n" CL_RESET);
			#endif
			break;
	}
	return result;
}

/************************************************************************
*																		*
*  Пытаемся увеличить умение персонажа.									*
*  Desynthesis (разбор предметов на запчасть) не увеличивает умение.	*
*																		*
*  Ломать не строить. Ломание столов и стульев не сделает из нас		*
*  плотника, значит умение в данном случае не повышается.				*
*																		*
************************************************************************/

int32 doSynthSkillUp(CCharEntity* PChar) 
{
	//if (PChar->Container->getType() == ELEMENT_LIGHTNING)
	//{
	//	return 0;
	//} bad idea, you cannot synth any item with lightning crystal

	double MoonPhase = (double)CVanaTime::getInstance()->getMoonPhase();
	double MoonCorrection = MoonPhase / 500;

	for(uint8 skillID = 49; skillID < 57; ++skillID) 
	{
		if (PChar->Container->getQuantity(skillID-40) == 0)	// получаем необходимый уровень умения рецепта
		{
			continue;
		}

		uint8  skillRank = PChar->RealSkills.rank[skillID];
		uint16 maxSkill  = (skillRank+1)*100;

		int32  charSkill = PChar->RealSkills.skill[skillID];
		int32  basDiff   = PChar->Container->getQuantity(skillID-40) - charSkill/10;
		double synthDiff = getSynthDifficulty(PChar, skillID);

		if ((basDiff <= 0) || ((basDiff > 5) && (PChar->Container->getQuantity(0) == SYNTHESIS_FAIL)))		// результат синтеза хранится в quantity нулевой ячейки
		{
			return 0;
		}

		if (charSkill < maxSkill)
		{
			double skillUpChance = (synthDiff*(map_config.craft_multiplier - (log(1.2 + charSkill/100) + MoonCorrection)))/10;
			skillUpChance = skillUpChance/(1 + (PChar->Container->getQuantity(0) == SYNTHESIS_FAIL));		// результат синтеза хранится в quantity нулевой ячейки

			double random = rand() / ((double)RAND_MAX);
			#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
			ShowDebug(CL_CYAN"Skill up chance: %g  Random: %g\n" CL_RESET, skillUpChance, random);
			#endif

			if (random < skillUpChance)
			{
				int32  satier = 0;
				int32  skillAmount  = 1;
				double chance = 0;

				if((synthDiff >= 1) && (synthDiff < 3)){
					satier = 1;
				}else if((synthDiff >= 3) && (synthDiff < 5)){
					satier = 2;
				}else if((synthDiff >= 5) && (synthDiff < 8)){
					satier = 3;
				}else if((synthDiff >= 8) && (synthDiff < 10)){
					satier = 4;
				}else if (synthDiff >= 10)
					satier = 5;
				//if (skillRank > 5)
				//	satier--;
					
				for(uint8 i = 0; i < 4; i ++) 
				{
					random = rand() / ((double)RAND_MAX);
					#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
					ShowDebug(CL_CYAN"SkillAmount Tier: %i  Random: %g\n" CL_RESET, satier, random);
					#endif
						
					switch(satier) 
					{
						case 5:  chance = 0.900; break;
						case 4:  chance = 0.700; break;
						case 3:  chance = 0.500; break;
						case 2:  chance = 0.300; break;
						case 1:  chance = 0.100; break;
						default: chance = 0.000; break;
					}
					if(chance < random)
						break;
					skillAmount += 1;
					satier -= 1;
				}

				if((skillAmount + charSkill) > maxSkill)
				{
					skillAmount = maxSkill - charSkill;
				}

				PChar->RealSkills.skill[skillID] += skillAmount; 
				PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, skillID, skillAmount, 38));

				if((charSkill/10) < (charSkill + skillAmount)/10) 
				{
					PChar->WorkingSkills.skill[skillID] += 0x20;

					PChar->pushPacket(new CCharSkillsPacket(PChar));
					PChar->pushPacket(new CMessageBasicPacket(PChar, PChar, skillID, (charSkill + skillAmount)/10, 53));	
				}

				charutils::SaveCharSkills(PChar, skillID);
			}
		}
	}
	return 0;
}

/************************************************************************
*																		*
*  Синтез завершился неудачей. Решаем вопрос, сколько ингредиентов		*
*  потеряет персонаж. Вероятность потери зависить от умения, из-за		*
*  которого синтез провалился. ID умения сохранен в slotID ячейки		*
*  кристалла.															*	
*																		*
************************************************************************/

int32 doSynthFail(CCharEntity* PChar) 
{
	uint8  carrentCraft = PChar->Container->getInvSlotID(0);
	double synthDiff    = getSynthDifficulty(PChar, carrentCraft);
	double moghouseAura = 0;

	if (PChar->getZone() == 0) // неправильное условие, т.к. аура действует лишь в собственном доме
	{
		// Проверяем элемент синтеза
		switch (PChar->Container->getType()) 
		{
			case ELEMENT_FIRE:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_FIRE);	   break;
			case ELEMENT_EARTH:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_EARTH);	   break;
			case ELEMENT_WATER:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_WATER);	   break;
			case ELEMENT_WIND:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_WIND);	   break;
			case ELEMENT_ICE:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_ICE);	   break;
			case ELEMENT_LIGHTNING:	moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_LIGHTNING); break;
			case ELEMENT_LIGHT:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_LIGHT);	   break;
			case ELEMENT_DARK:		moghouseAura = 0.05 * charutils::hasKeyItem(PChar,MOGHANCEMENT_DARK);	   break;
		}

		if (moghouseAura == 0)
		{
			switch (carrentCraft)
			{
				case SKILL_WDW:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_WOODWORKING);	 break;
				case SKILL_SMT:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_SMITHING);	 break;
				case SKILL_GLD:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_GOLDSMITHING); break;
				case SKILL_CLT:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_CLOTHCRAFT);	 break;
				case SKILL_LTH:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_LEATHERCRAFT); break;
				case SKILL_BON:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_BONECRAFT);	 break;
				case SKILL_ALC:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_ALCHEMY);		 break;
				case SKILL_COK:	 moghouseAura = 0.075 * charutils::hasKeyItem(PChar,MOGLIFICATION_COOKING);		 break;
			}
		}

		if (moghouseAura == 0)
		{
			switch (carrentCraft)
			{
				case SKILL_WDW:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_WOODWORKING);  break;
				case SKILL_SMT:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_SMITHING);		break;
				case SKILL_GLD:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_GOLDSMITHING);	break;
				case SKILL_CLT:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_CLOTHCRAFT);	break;
				case SKILL_LTH:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_LEATHERCRAFT);	break;
				case SKILL_BON:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_BONECRAFT);	break;
				case SKILL_ALC:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_ALCHEMY);		break;
				case SKILL_COK:	 moghouseAura = 0.1 * charutils::hasKeyItem(PChar,MEGA_MOGLIFICATION_COOKING);		break;
			}
		}
	}

	uint8 invSlotID  = 0;
	uint8 nextSlotID = 0;
	uint8 lostCount  = 0;
		
	double random   = 0;
	double lostItem = 0.15 - moghouseAura + (synthDiff > 0 ? synthDiff/20 : 0);

	invSlotID = PChar->Container->getInvSlotID(1);

	for(uint8 slotID = 1; slotID <= 8; ++slotID) 
	{
		if (slotID != 8)
			nextSlotID = PChar->Container->getInvSlotID(slotID+1);
		
		random = rand() / ((double) RAND_MAX);
		#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
		ShowDebug(CL_CYAN"Lost Item: %g  Random: %g\n" CL_RESET, lostItem, random);
		#endif

		if(random < lostItem) {
			PChar->Container->setQuantity(slotID, 0);
			lostCount++;
		}

		if(invSlotID != nextSlotID) 
		{
			CItem* PItem = PChar->getStorage(LOC_INVENTORY)->GetItem(invSlotID);

			if (PItem != NULL)
			{
				PItem->setSubType(ITEM_UNLOCKED);

				if(lostCount > 0) 
				{
					#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
					ShowDebug(CL_CYAN"Removing quantity %u from inventory slot %u\n" CL_RESET, lostCount, invSlotID);
					#endif
					
					charutils::UpdateItem(PChar, LOC_INVENTORY, invSlotID, -(int32)lostCount);
					lostCount = 0;
				}else{				
					PChar->pushPacket(new CInventoryAssignPacket(PItem, INV_NORMAL));
				}
			}
			invSlotID  = nextSlotID;
			nextSlotID = 0;
		}
		if(invSlotID == 0xFF)		
			break;
	}

    if(PChar->loc.zone->GetID() != 255 && PChar->loc.zone->GetID() != 0)
    {
        // Don't send this packet to the zone it does funky stuff.
        //PChar->loc.zone->PushPacket(PChar, CHAR_INRANGE, new CSynthMessagePacket(PChar, SYNTH_FAIL));

        PChar->pushPacket(new CSynthMessagePacket(PChar, SYNTH_FAIL));
    }
    else
    {
        PChar->pushPacket(new CSynthMessagePacket(PChar, SYNTH_FAIL));
    }

	return 0;
}

/************************************************************************
*																		*
*  Начало синтеза.														*
*  в поле type контейнера записываем элемент синтеза					*
*																		*
************************************************************************/

int32 startSynth(CCharEntity* PChar)
{
	uint16 effect  = 0;
	uint8  element = 0;

	uint16 crystalType = PChar->Container->getItemID(0);

	switch(crystalType) 
	{
		case 0x1000:
		case 0x108E:
			effect  = EFFECT_FIRESYNTH;
			element = ELEMENT_FIRE;
			break;
		case 0x1001:
		case 0x108F:
			effect  = EFFECT_ICESYNTH;
			element = ELEMENT_ICE;
			break;
		case 0x1002:
		case 0x1090:
			effect  = EFFECT_WINDSYNTH;
			element = ELEMENT_WIND;
			break;
		case 0x1003:
		case 0x1091:
			effect  = EFFECT_EARTHSYNTH;
			element = ELEMENT_EARTH;
			break;
		case 0x1004:
		case 0x1092:
			effect  = EFFECT_LIGHTNINGSYNTH;
			element = ELEMENT_LIGHTNING;
			break;
		case 0x1005:
		case 0x1093:
			effect  = EFFECT_WATERSYNTH;
			element = ELEMENT_WATER;
			break;
		case 0x1006:
		case 0x1094:
			effect  = EFFECT_LIGHTSYNTH;
			element = ELEMENT_LIGHT;
			break;
		case 0x1007:
		case 0x1095:
			effect  = EFFECT_DARKSYNTH;
			element = ELEMENT_DARK;
			break;
	}

	PChar->Container->setType(element);

	if (!isRightRecipe(PChar)) 
	{
		return 0;
	}

	// удаляем кристалл
	charutils::UpdateItem(PChar, LOC_INVENTORY, PChar->Container->getInvSlotID(0), -1);	

	uint8 result = calcSynthResult(PChar);

	uint8  invSlotID  = 0;
	uint8  tempSlotID = 0;
	uint16 itemID     = 0;
	uint32 quantity   = 0;	

	for(uint8 slotID = 1; slotID <= 8; ++slotID) 
	{
		tempSlotID = PChar->Container->getInvSlotID(slotID);
		if ((tempSlotID != 0xFF) && (tempSlotID != invSlotID)) 
		{
			invSlotID = tempSlotID;

			CItem* PItem = PChar->getStorage(LOC_INVENTORY)->GetItem(invSlotID);

			if (PItem != NULL)
			{
				PItem->setSubType(ITEM_LOCKED);
				PChar->pushPacket(new CInventoryAssignPacket(PItem, INV_NOSELECT));
			}
		}
	}

	PChar->animation = ANIMATION_SYNTH;
	PChar->pushPacket(new CCharUpdatePacket(PChar));

    if(PChar->loc.zone->GetID() != 255 && PChar->loc.zone->GetID() != 0)
    {
        PChar->loc.zone->PushPacket(PChar, CHAR_INRANGE_SELF, new CSynthAnimationPacket(PChar,effect,result));  
    }
    else
    {
        PChar->pushPacket(new CSynthAnimationPacket(PChar, effect, result));
    }
    
	return 0;
}

/************************************************************************
*																		*
*  Отправляем результат синтеза персонажу								*
*																		*
************************************************************************/

int32 doSynthResult(CCharEntity* PChar) 
{
	uint8 m_synthResult = PChar->Container->getQuantity(0);

	if (m_synthResult == SYNTHESIS_FAIL)
	{
		doSynthFail(PChar);
	}else{
		uint16 itemID   = PChar->Container->getItemID(10 + m_synthResult);
		uint8  quantity = PChar->Container->getInvSlotID(10 + m_synthResult); // к сожалению поле quantity занято

		uint8 invSlotID   = 0;
		uint8 nextSlotID  = 0;
		uint8 removeCount = 0;

		invSlotID = PChar->Container->getInvSlotID(1);
			
		for(uint8 slotID = 1; slotID <= 8; ++slotID) 
		{
			nextSlotID = (slotID != 8 ? PChar->Container->getInvSlotID(slotID+1) : 0);
			removeCount++;
				
			if (invSlotID != nextSlotID)
			{
				if (invSlotID != 0xFF)
				{
					#ifdef _DSP_SYNTH_DEBUG_MESSAGES_
					ShowDebug(CL_CYAN"Removing quantity %u from inventory slot %u\n" CL_RESET,removeCount,invSlotID);
					#endif
					PChar->getStorage(LOC_INVENTORY)->GetItem(invSlotID)->setSubType(ITEM_UNLOCKED);
					charutils::UpdateItem(PChar, LOC_INVENTORY, invSlotID, -(int32)removeCount); 
				}
				invSlotID   = nextSlotID;
				nextSlotID  = 0;
				removeCount = 0;
			}
		}

        // TODO: перейти на новую функцию AddItem, чтобы не обновлять signature ручками

		invSlotID = charutils::AddItem(PChar, LOC_INVENTORY, itemID, quantity);

		CItem* PItem = PChar->getStorage(LOC_INVENTORY)->GetItem(invSlotID);

		if (PItem != NULL)
		{
			if ((PItem->getFlag() & ITEM_FLAG_INSCRIBABLE) && (PChar->Container->getItemID(0) > 0x1080))
			{
                int8 encodedSignature [12];
				PItem->setSignature(EncodeStringSignature((int8*)PChar->name.c_str(), encodedSignature));

                int8 signature_esc[31]; //max charname: 15 chars * 2 + 1
				Sql_EscapeStringLen(SqlHandle,signature_esc,PChar->name.c_str(),strlen(PChar->name.c_str()));
				 
				int8* fmtQuery = "UPDATE char_inventory SET signature = '%s' WHERE charid = %u AND location = 0 AND slot = %u;\0";
				
				Sql_Query(SqlHandle,fmtQuery,signature_esc,PChar->id, invSlotID);
			}
			PChar->pushPacket(new CInventoryItemPacket(PItem, LOC_INVENTORY, invSlotID));
		}

		PChar->pushPacket(new CInventoryFinishPacket());
        if(PChar->loc.zone->GetID() != 255 && PChar->loc.zone->GetID() != 0)
        {
            // Don't send this packet to the zone it does funky stuff.
            //PChar->loc.zone->PushPacket(PChar, CHAR_INRANGE_SELF, new CSynthMessagePacket(PChar, SYNTH_SUCCESS, itemID, quantity));

            PChar->pushPacket(new CSynthMessagePacket(PChar, SYNTH_SUCCESS, itemID, quantity));
        }
        else
        {
            PChar->pushPacket(new CSynthMessagePacket(PChar, SYNTH_SUCCESS, itemID, quantity));
        }
	}

	doSynthSkillUp(PChar);

	return 0;
}

/************************************************************************
*																		*
*  Завершаем синтез 													*
*																		*
************************************************************************/

int32 sendSynthDone(CCharEntity* PChar) 
{	
	doSynthResult(PChar);

	PChar->animation = ANIMATION_NONE;
	PChar->pushPacket(new CCharUpdatePacket(PChar));
	return 0;
}

} // namespace synth