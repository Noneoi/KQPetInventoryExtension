package mmo.activityext.newact20240705.nationalcompetition.config
{
   import mmo.play.compatible.H5;
   
   public class NC_Config
   {
      
      private static const PRE_URL:String = H5.s("activityext/20240705/nationalcompetition/nationalcompetition","newactivityext/newact20240705/nationalcompetition/nationalcompetition_");
      
      private static const PRE_CLS:String = "mmo.activityext.newact20240705.nationalcompetition.";
      
      public static const WIN_NUM:int = 8;
      
      public static const FAIL_NUM:int = 3;
      
      public static const CHOOSE_PET_NUM:int = 5;
      
      public static const STATE_NOT_START:int = 0;
      
      public static const STATE_RONDOM_FORMATION:int = 1;
      
      public static const STATE_CHOOSE_PET:int = 2;
      
      public static const STATE_BATTLE:int = 3;
      
      public static const STATE_GET_BONUS:int = 4;
      
      public static const STATE_CHOOSE_BUFF:int = 5;
      
      public static const STATE_RESET_FORMATION:int = 6;
      
      public static const STATE_RECHOOSE_FORMATION:int = 8;
      
      public static const STATE_CHOOSE_REPLNC_E_PET:int = 7;
      
      public static const MATCH_TIME:int = 30;
      
      public static const NUM_TOTAL_COUNTS:int = 10000;
      
      public static const NUM_PER_PAGE:int = 5;
      
      public static const ACT_LINK:String = "btnNewAct_nationalcompetition_showMainPanel_clearAll";
      
      public static const BUFF_DETAIL:Array = [{
         "id":0,
         "name":"name",
         "effectType":"effectType",
         "describe":"describe",
         "handler":-1
      },{
         "id":1,
         "name":"布阵（弃用）",
         "effectType":1,
         "describe":"重新布置阵型",
         "handler":-1
      },{
         "id":2,
         "name":"换阵布阵（弃用）",
         "effectType":2,
         "describe":"重新随机阵型并布置",
         "handler":-1
      },{
         "id":3,
         "name":"全阵增伤",
         "effectType":3,
         "describe":"己阵全阵精灵永久获得伤害提升12%",
         "handler":-1
      },{
         "id":4,
         "name":"全阵减伤",
         "effectType":3,
         "describe":"己阵全阵精灵永久获得受伤减少10%",
         "handler":-1
      },{
         "id":5,
         "name":"后排增伤",
         "effectType":4,
         "describe":"己阵各横排站位最后的精灵永久获得伤害提升25%",
         "handler":-1
      },{
         "id":6,
         "name":"前排减伤",
         "effectType":4,
         "describe":"己阵各横排站位最前的精灵永久获得受伤减少20%",
         "handler":-1
      },{
         "id":7,
         "name":"神速五免（弃用）",
         "effectType":4,
         "describe":"己阵站位最前的神速精灵永久获得五免，并获得[免疫]（持续1次攻击）",
         "handler":-1
      },{
         "id":8,
         "name":"神速降气",
         "effectType":4,
         "describe":"进入战斗时，己阵站位最后的神速精灵令敌阵同横排所有精灵降低气势100点",
         "handler":-1
      },{
         "id":9,
         "name":"神攻免伤（弃用）",
         "effectType":4,
         "describe":"己阵站位最前的神攻精灵获得[免伤]（持续2次攻击）",
         "handler":-1
      },{
         "id":10,
         "name":"神攻隐身",
         "effectType":4,
         "describe":"己阵站位最后的神攻精灵获得[隐身]（持续1个大回合），且伤害永久提升50%",
         "handler":-1
      },{
         "id":11,
         "name":"神盾强击（弃用）",
         "effectType":4,
         "describe":"己阵站位最前的神盾获得[强力头槌]（持续2回合）：\n出手后令目标获得[强力枷锁]（持续1回合）",
         "handler":-1
      },{
         "id":12,
         "name":"神盾先攻（弃用）",
         "effectType":4,
         "describe":"进入战斗时，己阵站位最前的神盾精灵立刻出手1次",
         "handler":-1
      },{
         "id":13,
         "name":"群攻连携",
         "effectType":3,
         "describe":"己阵精灵群攻时，有35%概率令自身获得[连携]（每位精灵可触发1次）",
         "handler":-1
      },{
         "id":14,
         "name":"群体免疫（弃用）",
         "effectType":3,
         "describe":"进入战斗时，己阵精灵获得[免疫]（全阵合计可触发3次）",
         "handler":-1
      },{
         "id":15,
         "name":"逆克制毁伤（弃用）",
         "effectType":3,
         "describe":"己阵精灵攻击逆属性克制的精灵时，令目标获得[毁灭伤害·Ⅱ]",
         "handler":-1
      },{
         "id":16,
         "name":"以下克上",
         "effectType":3,
         "describe":"己阵精灵攻击极限战斗力高于自身的精灵时，吸收敌阵1点通灵之魂",
         "handler":-1
      },{
         "id":17,
         "name":"通灵回气（弃用）",
         "effectType":3,
         "describe":"己阵通灵师触发通灵时，令己阵存活的所有精灵恢复满气势",
         "handler":-1
      },{
         "id":18,
         "name":"热身运动（弃用）",
         "effectType":3,
         "describe":"己阵全阵精灵获得伤害提升25%、受伤减少20%（己阵通灵师触发通灵后移除该全阵效果）",
         "handler":-1
      },{
         "id":19,
         "name":"我们都是龙",
         "effectType":3,
         "describe":"己阵[龙尊权能]可触发数量无限制",
         "handler":-1
      },{
         "id":20,
         "name":"跟你爆了（弃用）",
         "effectType":3,
         "describe":"己阵精灵若在首次出手前死亡，则令敌阵同横排所有精灵获得[毁灭伤害·Ⅱ]",
         "handler":-1
      },{
         "id":21,
         "name":"毁灭终结（弃用）",
         "effectType":3,
         "describe":"第一个大回合结束时，每存活1只己阵精灵对敌阵全阵精灵造成1次8万点毁灭伤害",
         "handler":-1
      },{
         "id":22,
         "name":"激情王八拳",
         "effectType":3,
         "describe":"双方阵型所有精灵获得[嘲讽]（持续99回合），\n且无法获得[连击]、[二段连击]、[连携]",
         "handler":-1
      },{
         "id":23,
         "name":"你吃了没（弃用）",
         "effectType":3,
         "describe":"己阵精灵受到攻击后若拥有[护盾]，获得2点通灵之魂",
         "handler":-1
      },{
         "id":24,
         "name":"三元之力",
         "effectType":4,
         "describe":"若己阵使用的精灵属性种类，包括神光、神暗、神灵、神无极\n中的3种及以上，则每个大回合开始时获得6点通灵之魂",
         "handler":-1
      },{
         "id":25,
         "name":"三灵之力",
         "effectType":4,
         "describe":"若己阵使用的精灵属性种类，包括神火、神水、神草、神无极\n中的3种及以上，则每个大回合开始时获得6点通灵之魂",
         "handler":-1
      },{
         "id":26,
         "name":"有福同享（弃用）",
         "effectType":4,
         "describe":"己阵站位最前的神速精灵令同横排的所有己阵精灵\n获得100点气势与[免疫]（持续1次攻击）",
         "handler":-1
      },{
         "id":27,
         "name":"有难我当（弃用）",
         "effectType":4,
         "describe":"己阵站位最后的神盾精灵提升生命值与生命值\n上限100%，并获得[嘲讽]（持续3次攻击）",
         "handler":-1
      },{
         "id":28,
         "name":"以眼还眼（弃用）",
         "effectType":3,
         "describe":"己阵精灵触发免疫后，令攻击来源获得[毁灭伤害·Ⅱ]",
         "handler":-1
      },{
         "id":29,
         "name":"阿尔法之力（弃用）",
         "effectType":4,
         "describe":"如己阵女性精灵数量>4，进入战斗立即获得5点通灵之魂",
         "handler":-1
      },{
         "id":30,
         "name":"人少势众（弃用）",
         "effectType":3,
         "describe":"进入战斗时，如己阵精灵数量少于敌方，己阵精灵死亡时可立即重生（可触发2次）",
         "handler":-1
      },{
         "id":31,
         "name":"神盾免疫",
         "effectType":4,
         "describe":"己阵站位最前的神盾精灵令己阵同横排的精灵获得[免疫]（持续1次攻击）",
         "handler":-1
      },{
         "id":32,
         "name":"神攻加气（弃用）",
         "effectType":4,
         "describe":"进入战斗时，己阵站位最前的神攻精灵获得150点气势",
         "handler":-1
      },{
         "id":33,
         "name":"神盾嘲讽（弃用）",
         "effectType":4,
         "describe":"己阵站位最后的神盾精灵受伤减少20%，并获得[嘲讽]（持续3次攻击）",
         "handler":-1
      },{
         "id":34,
         "name":"守梦之力",
         "effectType":3,
         "describe":"己阵神攻精灵完整出手后，获得2点通灵之魂（每位精灵可触发1次）",
         "handler":-1
      },{
         "id":35,
         "name":"复苏之力（弃用）",
         "effectType":3,
         "describe":"己阵精灵首次恢复生命时，额外恢复满气势",
         "handler":-1
      },{
         "id":36,
         "name":"弥时之力",
         "effectType":4,
         "describe":"若己阵使用的精灵属性种类，包括神火、神幻、神光、神无极\n中的3种及以上，则每个大回合开始时获得6点通灵之魂",
         "handler":-1
      },{
         "id":37,
         "name":"沉着命中（弃用）",
         "effectType":4,
         "describe":"己阵第一纵列的精灵出手前命中率提升100%（持续1次攻击）",
         "handler":-1
      },{
         "id":38,
         "name":"毁灭之力（弃用）",
         "effectType":3,
         "describe":"己阵精灵首次闪避时，对攻击者造成[额外伤害·Ⅰ]",
         "handler":-1
      },{
         "id":39,
         "name":"精准打击（弃用）",
         "effectType":4,
         "describe":"己阵各横排首位精灵出手后，若目标血量未减少，令目标获得[毁灭伤害·Ⅱ]",
         "handler":-1
      },{
         "id":40,
         "name":"潮汐之力（弃用）",
         "effectType":4,
         "describe":"进入战斗时，己阵获得1点[潮汐之力]",
         "handler":-1
      },{
         "id":41,
         "name":"幸运轮盘·天煞",
         "effectType":4,
         "describe":"进入战斗时，己阵随机3位精灵获得8级天煞孤星的效果；\n[8级天煞孤星]：攻击1位目标后，令目标降低气势:{100点}并受到{50万}点毁灭伤害（非负面，仅触发1次）",
         "handler":-1
      },{
         "id":42,
         "name":"幸运轮盘·福虎",
         "effectType":4,
         "describe":"进入战斗时，己阵随机4位精灵获得8级福虎佑灵的效果；\n[8级福虎佑灵]：首次受到攻击时，提升超物防和超魔防:{0%}。提升双防：{48%}（持续1个大回合）",
         "handler":-1
      },{
         "id":43,
         "name":"诺雅·皎月之力（弃用）",
         "effectType":4,
         "describe":"进入战斗时，若己阵生效神运诺雅的通灵技，则所有女性精灵获得气势100点",
         "handler":-1
      },{
         "id":44,
         "name":"究·多彩之力（弃用）",
         "effectType":4,
         "describe":"进入战斗时，若己阵生效神运究的通灵技，则令神运究获得【多彩之力】：完整出手后，己阵存活精灵中每有2种神属性就获得1点通灵之魂",
         "handler":-1
      },{
         "id":45,
         "name":"冷板凳逆袭（弃用）",
         "effectType":4,
         "describe":"己阵极限战斗力最低1位精灵获得[冷板凳逆袭]：己阵完整回合结束后，获得双攻、双防提升5万点；自身首次出手后，不再获得属性提升",
         "handler":-1
      },{
         "id":46,
         "name":"溯灵强化（弃用）",
         "effectType":4,
         "describe":"己阵所有精灵随机获得以下效果之一：400点[格挡之力]；200点[暴击之力]；1500点[破击之力]；40点[防暴之力]；",
         "handler":-1
      },{
         "id":47,
         "name":"神速刺客",
         "effectType":4,
         "describe":"己阵所有神速精灵获得以下效果：伤害提升20%，优先攻击敌阵站位最后的神攻或神速精灵（群攻时不生效）",
         "handler":-1
      },{
         "id":48,
         "name":"昆吾·创始之力（弃用）",
         "effectType":4,
         "describe":"若己阵生效神运昆吾的通灵技，则己阵昆吾获得以下效果：伤害提升50%；攻击极限战斗力大于等于自身的精灵前，令目标获得3层[崩甲]，并降低敌阵1点通灵之魂",
         "handler":-1
      },{
         "id":49,
         "name":"昆仑·创界之力",
         "effectType":4,
         "describe":"若己阵生效灵初昆仑的通灵技，则己阵获得以下效果：己阵精灵死亡后，若己阵存活精灵数量≤2，则吸收敌阵12点通灵之魂，并令灵初昆仑获得1次[连携储备]（仅触发1次）",
         "handler":-1
      },{
         "id":50,
         "name":"尼莫妮·海洋之力（弃用）",
         "effectType":4,
         "describe":"若己阵生效灵初尼莫妮的通灵技，且己阵使用的神平衡精灵少于等于2位，则己阵获得以下效果：己阵完整回合结束后，获得4点通灵之魂",
         "handler":-1
      },{
         "id":51,
         "name":"命运馈赠",
         "effectType":3,
         "describe":"己阵所有精灵获得以下效果：进入战斗时，若敌阵同站位没有使用精灵（不包含分身、天觉者契约精灵等），则获得两点通灵之魂",
         "handler":-1
      },{
         "id":52,
         "name":"力量觉醒",
         "effectType":4,
         "describe":"每3个己阵完整回合结束后，双方所有精灵获得双攻提升5万点，己阵额外获得2点通灵之魂",
         "handler":-1
      },{
         "id":53,
         "name":"天命既定",
         "effectType":3,
         "describe":"己阵所有精灵获得以下效果：攻击前，有5%概率获得[瞬杀]（持续1次攻击）",
         "handler":-1
      },{
         "id":54,
         "name":"无敌·神威无上",
         "effectType":3,
         "describe":"若己阵生效灵初无敌的通灵技，则己阵无敌获得以下效果：攻击前，令目标获得1层[崩甲]",
         "handler":-1
      },{
         "id":55,
         "name":"幸运轮盘·乘胜（弃用）",
         "effectType":4,
         "describe":"进入战斗时，己阵随机2位精灵获得8级乘胜追击的效果；\n[8级乘胜追击]：己阵任意精灵释放超杀后，自身立刻获得:{200点[暴击之力]}（可触发2次）",
         "handler":-1
      },{
         "id":56,
         "name":"幸运轮盘·反守",
         "effectType":4,
         "describe":"进入战斗时，己阵随机3位精灵获得8级反守为攻的效果；\n[8级反守为攻]： 降低双防{6%}，提升暴击率18%、命中率24%、破击率24%，提升双攻{9%}",
         "handler":-1
      },{
         "id":57,
         "name":"尼莫妮·远航之力",
         "effectType":4,
         "describe":"若己阵生效灵初尼莫妮的通灵技，则己阵神平衡职业精灵完整回合后获得2点通灵之魂",
         "handler":-1
      },{
         "id":58,
         "name":"毁灭打击",
         "effectType":4,
         "describe":"第一个大回合结束时，每存活1只己阵精灵对敌阵全阵精灵造成1次30万点毁灭伤害",
         "handler":-1
      },{
         "id":59,
         "name":"格挡移除",
         "effectType":3,
         "describe":"己阵精灵出手前，移除攻击目标100点[格挡之力]（持续1个大回合）",
         "handler":-1
      },{
         "id":60,
         "name":"未来·无限变量",
         "effectType":3,
         "describe":"若己阵生效灵初未来通灵技，则己阵男性精灵完整出手后获得5点通灵之魂（每位精灵每个大回合仅触发一次）",
         "handler":-1
      },{
         "id":61,
         "name":"瑟柏娜·蛇心之护",
         "effectType":4,
         "describe":"若己阵生效灵初瑟柏娜通灵技，进入战斗时己阵所有精灵获得300点[格挡之力]",
         "handler":-1
      },{
         "id":62,
         "name":"法纳斯·万灵寂语",
         "effectType":4,
         "describe":"进入战斗时，若己阵生效灵初法纳斯通灵技，则己阵进入战斗时获得1000点[闪避灵力]",
         "handler":-1
      },{
         "id":63,
         "name":"黛安娜·封魔镇魂",
         "effectType":4,
         "describe":"若己阵生效灵初黛安娜通灵技，则己阵神速精灵每次出手后获得2点通灵之魂",
         "handler":-1
      },{
         "id":64,
         "name":"力量之王",
         "effectType":4,
         "describe":"进入战斗时，己阵站位最靠前的精灵获得[无敌战神]：受到的任何伤害固定减少1000000点（持续2个大回合）",
         "handler":-1
      },{
         "id":65,
         "name":"暴击提升（弃用）",
         "effectType":4,
         "describe":"进入战斗时，己阵所有精灵获得1次[暴击蓄力·66%]",
         "handler":-1
      },{
         "id":66,
         "name":"究·万灵俯首",
         "effectType":4,
         "describe":"若己阵生效灵初究通灵技，且己阵极限战力总和大于敌阵极限战力总和，则己阵获得14点通灵之魂",
         "handler":-1
      },{
         "id":67,
         "name":"暴击提升",
         "effectType":4,
         "describe":"进入战斗时，己阵随机2位精灵暴击率提升80%（优先选择神攻神速职业的精灵）",
         "handler":-1
      },{
         "id":68,
         "name":"防暴提升",
         "effectType":4,
         "describe":"进入战斗时，己阵随机2位精灵防暴率提升66%（优先选择神盾神平衡职业的精灵）",
         "handler":-1
      },{
         "id":69,
         "name":"团队核心",
         "effectType":4,
         "describe":"进入战斗时，若己阵极限战力最低的精灵≤30000，则该精灵获得[团队核心]：双防降低60万点，血量及血量上限修改为1点，且不会恢复血量与获得灵盾值，己阵精灵受到伤害后，获得100万点灵盾值（每个大回合至多可触发10次，阵亡后失去该效果）",
         "handler":-1
      }];
      
      public static const MAX_TICKET:int = 40;
      
      public static const DAILY_MAX_TICKET:int = 40;
      
      public static const COST_TICKET:Array = [{
         "buyTimes":1,
         "cost":10
      }];
      
      public static const WIN_NUM_MAP_SCORE:Array = [{
         "winTimes":0,
         "score":20
      },{
         "winTimes":1,
         "score":60
      },{
         "winTimes":2,
         "score":110
      },{
         "winTimes":3,
         "score":170
      },{
         "winTimes":4,
         "score":240
      },{
         "winTimes":5,
         "score":320
      },{
         "winTimes":6,
         "score":410
      },{
         "winTimes":7,
         "score":510
      },{
         "winTimes":8,
         "score":1000
      }];
      
      public static const SPEC_RNC_E_ID_BEGIN:int = 1000;
      
      public static const RANK_TOP_REWARD:String = "120:1443:1";
      
      public static const RANK_TOP_TITLE:String = "106:698:1";
      
      public static const SEASON_COUNT:int = 24;
      
      public static const SEASON_START_DATE:String = "20261009";
      
      public static const SEASON_END_DATE_INCLUDED:String = "20261112";
      
      public function NC_Config()
      {
         super();
      }
      
      public static function url(param1:String) : String
      {
         return PRE_URL + param1;
      }
      
      public static function cls(param1:String) : String
      {
         return param1;
      }
      
      public static function clsWithPre(param1:String) : String
      {
         return PRE_CLS + param1;
      }
      
      public static function get defautBuffConfig() : Array
      {
         return BUFF_DETAIL.concat();
      }
      
      public static function get winNumMapScore() : Array
      {
         return WIN_NUM_MAP_SCORE.concat();
      }
      
      public static function get costTicketConfig() : Array
      {
         return COST_TICKET.concat();
      }
      
      public static function getCost(param1:int) : int
      {
         var _loc2_:int = 0;
         while(_loc2_ < costTicketConfig.length)
         {
            if(param1 < costTicketConfig[_loc2_]["buyTimes"])
            {
               return COST_TICKET[param1]["cost"];
            }
            _loc2_++;
         }
         return costTicketConfig[costTicketConfig.length - 1]["cost"];
      }
   }
}

